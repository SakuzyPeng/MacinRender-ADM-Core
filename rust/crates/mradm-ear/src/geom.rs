use nalgebra::Vector3;
pub type Vec3 = Vector3<f64>;
pub fn cart(p: [f64; 3]) -> Vec3 {
    let a = -p[0] * (std::f64::consts::PI / 180.0);
    let e = p[1] * (std::f64::consts::PI / 180.0);
    Vec3::new(
        a.sin() * e.cos() * p[2],
        a.cos() * e.cos() * p[2],
        e.sin() * p[2],
    )
}
pub fn azimuth(p: Vec3) -> f64 {
    -p.x.atan2(p.y).to_degrees()
}
pub fn elevation(p: Vec3) -> f64 {
    p.z.atan2(p.x.hypot(p.y)).to_degrees()
}
pub fn inside_angle(mut x: f64, start: f64, mut end: f64, tol: f64) -> bool {
    // rem_euclid also bounds work for finite but very large authored angles.
    let span = end - start;
    if span > 360.0 || span < 0.0 {
        end = start + span.rem_euclid(360.0);
        if span > 0.0 && end == start {
            end += 360.0;
        }
    }
    x = start - tol + (x - (start - tol)).rem_euclid(360.0);
    x <= end + tol
}
pub fn vertex_order(points: &[Vec3]) -> Vec<usize> {
    let centre = points.iter().copied().sum::<Vec3>() / points.len() as f64;
    let a = points[0] - centre;
    let b = points
        .iter()
        .skip(1)
        .map(|p| *p - centre)
        .min_by(|x, y| x.dot(&a).abs().total_cmp(&y.dot(&a).abs()))
        .unwrap();
    let mut order: Vec<_> = (0..points.len()).collect();
    let angle = |i: usize| {
        let p = points[i] - centre;
        p.dot(&a).atan2(p.dot(&b))
    };
    order.sort_by(|&i, &j| angle(i).total_cmp(&angle(j)));
    order
}
pub fn normalize(g: &mut [f64]) {
    let norm = g.iter().map(|x| x * x).sum::<f64>().sqrt();
    for v in g {
        *v /= norm;
    }
}
pub fn interp(x: f64, xs: &[f64], ys: &[f64]) -> f64 {
    if x <= xs[0] {
        return ys[0];
    }
    for i in 1..xs.len() {
        if x < xs[i] {
            let t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
            return ys[i - 1] + t * (ys[i] - ys[i - 1]);
        }
    }
    ys[ys.len() - 1]
}
