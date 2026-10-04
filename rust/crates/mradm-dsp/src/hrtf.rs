//! Sparse HRTF interpolation grid. The immutable query BVH is shared; every
//! build owns its assignment state. Complexity follows intersected tree nodes,
//! rather than evaluating every measurement triangle at every grid point.
use crate::{Error, Result, geometry::*, vbap::Panner};
use std::sync::OnceLock;

pub const AZIMUTHS: usize = 361;
pub const ELEVATIONS: usize = 181;
pub const GRID_POINTS: usize = AZIMUTHS * ELEVATIONS;
#[derive(Clone)]
struct Query {
    direction: Vec3,
    index: usize,
}
struct Node {
    low: Vec3,
    high: Vec3,
    begin: usize,
    end: usize,
    children: Option<(usize, usize)>,
}
struct Tree {
    queries: Vec<Query>,
    nodes: Vec<Node>,
}
impl Tree {
    fn new() -> Self {
        let queries = (0..GRID_POINTS)
            .map(|index| Query {
                index,
                direction: direction(
                    (index % AZIMUTHS) as f32 - 180.,
                    (index / AZIMUTHS) as f32 - 90.,
                ),
            })
            .collect();
        let mut tree = Self {
            queries,
            nodes: Vec::with_capacity(GRID_POINTS / 4),
        };
        tree.build(0, GRID_POINTS);
        tree
    }
    fn build(&mut self, begin: usize, end: usize) -> usize {
        let mut low = [f64::INFINITY; 3];
        let mut high = [f64::NEG_INFINITY; 3];
        for q in &self.queries[begin..end] {
            for k in 0..3 {
                low[k] = low[k].min(q.direction[k]);
                high[k] = high[k].max(q.direction[k]);
            }
        }
        let id = self.nodes.len();
        self.nodes.push(Node {
            low,
            high,
            begin,
            end,
            children: None,
        });
        if end - begin > 16 {
            let axis = (0..3)
                .max_by(|&a, &b| (high[a] - low[a]).total_cmp(&(high[b] - low[b])))
                .unwrap();
            let middle = begin + (end - begin) / 2;
            self.queries[begin..end].select_nth_unstable_by(middle - begin, |a, b| {
                a.direction[axis]
                    .total_cmp(&b.direction[axis])
                    .then(a.index.cmp(&b.index))
            });
            let left = self.build(begin, middle);
            let right = self.build(middle, end);
            self.nodes[id].children = Some((left, right));
        }
        id
    }
}
struct Builder<'a> {
    tree: &'a Tree,
    assigned: Vec<bool>,
    remaining: Vec<usize>,
    measurements: usize,
    weights: &'a mut [f32],
    indices: &'a mut [i32],
}
impl Builder<'_> {
    fn visit(&mut self, id: usize, t: &Triplet) -> usize {
        let node = &self.tree.nodes[id];
        if self.remaining[id] == 0 {
            return 0;
        }
        for row in t.inverse {
            let upper: f64 = (0..3)
                .map(|k| {
                    row[k]
                        * if row[k] < 0. {
                            node.low[k]
                        } else {
                            node.high[k]
                        }
                })
                .sum();
            if upper < -0.001 - 1e-12 {
                return 0;
            }
        }
        let mut used = 0;
        if let Some((left, right)) = node.children {
            used = self.visit(left, t) + self.visit(right, t);
        } else {
            for query in &self.tree.queries[node.begin..node.end] {
                if self.assigned[query.index] {
                    continue;
                }
                let gains = t.inverse.map(|row| dot(row, query.direction));
                if gains.iter().any(|&v| v <= -0.001) || norm(gains) == 0. {
                    continue;
                }
                let mut values =
                    std::array::from_fn::<_, 3, _>(|i| (t.vertices[i], gains[i].max(0.)));
                values.sort_by_key(|v| v.0);
                let sum: f64 = values
                    .iter()
                    .filter(|v| v.0 < self.measurements && v.1 > 1e-7 * norm(gains))
                    .map(|v| v.1)
                    .sum();
                if sum > 0. {
                    let mut lane = 0;
                    for (index, gain) in values {
                        if index < self.measurements && gain > 1e-7 * norm(gains) {
                            self.indices[query.index * 3 + lane] = index as i32;
                            self.weights[query.index * 3 + lane] = (gain / sum) as f32;
                            lane += 1;
                        }
                    }
                }
                self.assigned[query.index] = true;
                used += 1;
            }
        }
        self.remaining[id] -= used;
        used
    }
}

pub fn grid_into(dirs: &[f32], weights: &mut [f32], indices: &mut [i32]) -> Result<()> {
    if weights.len() != GRID_POINTS * 3 || indices.len() != GRID_POINTS * 3 || dirs.len() < 8 {
        return Err(Error::InvalidArgument("Invalid HRTF grid buffers"));
    }
    let panner = Panner::new(dirs, true)?;
    static TREE: OnceLock<Tree> = OnceLock::new();
    let tree = TREE.get_or_init(Tree::new);
    weights.fill(0.);
    indices.fill(0);
    let mut builder = Builder {
        tree,
        assigned: vec![false; GRID_POINTS],
        remaining: tree.nodes.iter().map(|n| n.end - n.begin).collect(),
        measurements: dirs.len() / 2,
        weights,
        indices,
    };
    for triangle in &panner.triangles {
        builder.visit(0, triangle);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn full_sphere_grid_reconstructs_directions() {
        let dirs = [0., 0., 90., 0., 180., 0., -90., 0., 0., 90., 0., -90.];
        let vertices = directions(&dirs).unwrap();
        let mut weights = vec![0.; GRID_POINTS * 3];
        let mut ids = vec![0; GRID_POINTS * 3];
        grid_into(&dirs, &mut weights, &mut ids).unwrap();
        for index in (0..GRID_POINTS).step_by(7) {
            let sum: f32 = weights[index * 3..index * 3 + 3].iter().sum();
            assert!((sum - 1.).abs() < 1e-5);
            let reconstructed = (0..3).fold([0.; 3], |sum, k| {
                add(
                    sum,
                    scale(
                        vertices[ids[index * 3 + k] as usize],
                        weights[index * 3 + k] as f64,
                    ),
                )
            });
            let expected = direction(
                (index % AZIMUTHS) as f32 - 180.,
                (index / AZIMUTHS) as f32 - 90.,
            );
            assert!(norm(sub(unit(reconstructed), expected)) < 0.002);
        }
    }
}
