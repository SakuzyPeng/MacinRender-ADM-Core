//! Export writeback (PCM consistency 4.4): the semantic differences between the scene as imported
//! and as the policy left it, patched into the source AXML. Moved from the C++
//! `metadata.cpp` `collect_patches` with the same field mapping, consistency checks, check order
//! and messages; the patching itself is `mradm_adm::Document::apply_patches`. Positions, user
//! levels and HOA pack gain or mute are not written back.
use crate::error::{Context, Error, Result};
use crate::import::RenderScene;
use crate::policy::Overlay;
use mradm_adm::records::Patch;
use mradm_adm::{Document, Snapshot};

fn invalid(message: &str) -> Error {
    Error::invalid(message, Context::None)
}

fn require(condition: bool, message: &str) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(invalid(message))
    }
}

/// One scene state: its records and, for the imported scenes, the policy state beside them.
#[derive(Clone, Copy)]
struct Side<'a> {
    s: &'a Snapshot,
    overlay: Option<&'a Overlay>,
}

impl Side<'_> {
    fn string(&self, id: u32) -> &str {
        &self.s.strings[id as usize]
    }

    fn object_mute(&self, index: usize) -> bool {
        self.overlay
            .map_or(self.s.objects[index].mute != 0, |o| o.objects[index].mute)
    }
}

fn same_size(a: usize, b: usize, c: usize) -> Result<()> {
    require(a == b && b == c, "ADM 场景结构不一致")
}

fn same_time(times: [(u64, u64); 3]) -> Result<()> {
    let [b, o, e] = times;
    require(b == o && o == e, "ADM 时间线结构不一致")
}

/// A field value as the C++ scene held it: floats, flags and present-or-absent optionals.
#[derive(Clone, Copy, PartialEq)]
enum Value {
    Float(f32),
    Flag(bool),
    Distance(Option<f32>),
    Samples(Option<u64>),
}

#[derive(Default)]
struct Patches(Vec<Patch>);

impl Patches {
    fn add(&mut self, node: u32, field: u32, [b, o, e]: [Value; 3]) -> Result<()> {
        let number = |v: Value| match v {
            Value::Float(x) => f64::from(x),
            Value::Flag(x) => f64::from(u8::from(x)),
            Value::Distance(x) => x.map_or(0., f64::from),
            Value::Samples(_) => 0.,
        };
        let present = |v: Value| match v {
            Value::Distance(x) => u32::from(x.is_some()),
            Value::Samples(x) => u32::from(x.is_some()),
            _ => 1,
        };
        let samples = |v: Value| match v {
            Value::Samples(x) => x.unwrap_or(0),
            _ => 0,
        };
        let optional = matches!(b, Value::Distance(_) | Value::Samples(_));
        require(
            b == o,
            if optional {
                "原场景与源 AXML 可选字段不一致"
            } else {
                "原场景与源 AXML 语义不一致"
            },
        )?;
        self.0.push(Patch {
            node,
            field,
            present: present(e),
            original_present: present(o),
            value: number(e),
            original: number(o),
            samples: samples(e),
            original_samples: samples(o),
        });
        Ok(())
    }
}

fn flag(v: u32) -> Value {
    Value::Flag(v != 0)
}

fn collect(baseline: Side, original: Side, effective: Side) -> Result<Patches> {
    let (b, o, e) = (baseline.s, original.s, effective.s);
    same_size(b.objects.len(), o.objects.len(), e.objects.len())?;
    same_size(b.hoa.len(), o.hoa.len(), e.hoa.len())?;
    let mut patches = Patches::default();
    for oi in 0..b.objects.len() {
        let (bo, oo, eo) = (&b.objects[oi], &o.objects[oi], &e.objects[oi]);
        require(
            baseline.string(bo.id) == original.string(oo.id)
                && original.string(oo.id) == effective.string(eo.id)
                && bo.end == oo.end
                && oo.end == eo.end,
            "ADM 对象身份或时间不一致",
        )?;
        patches.add(bo.node, 1, [bo.gain, oo.gain, eo.gain].map(Value::Float))?;
        patches.add(
            bo.node,
            2,
            [
                baseline.object_mute(oi),
                original.object_mute(oi),
                effective.object_mute(oi),
            ]
            .map(Value::Flag),
        )?;
        same_size(bo.tracks.len, oo.tracks.len, eo.tracks.len)?;
        for ti in 0..bo.tracks.len {
            let bt = b.tracks[bo.tracks.offset + ti];
            let ot = o.tracks[oo.tracks.offset + ti];
            let et = e.tracks[eo.tracks.offset + ti];
            require(
                baseline.string(bt.uid) == original.string(ot.uid)
                    && original.string(ot.uid) == effective.string(et.uid),
                "ADM 轨道顺序不一致",
            )?;
            same_size(bt.blocks.len, ot.blocks.len, et.blocks.len)?;
            same_size(bt.ds_blocks.len, ot.ds_blocks.len, et.ds_blocks.len)?;
            for bi in 0..bt.blocks.len {
                let bb = &b.object_blocks[bt.blocks.offset + bi];
                let ob = &o.object_blocks[ot.blocks.offset + bi];
                let eb = &e.object_blocks[et.blocks.offset + bi];
                same_time([bb, ob, eb].map(|x| (x.start, x.end)))?;
                let blocks = [bb, ob, eb];
                let floats = |f: fn(&mradm_adm::records::ObjectBlock) -> f32| {
                    blocks.map(|x| Value::Float(f(x)))
                };
                let node = bb.node;
                patches.add(node, 1, floats(|x| x.gain))?;
                patches.add(node, 3, blocks.map(|x| flag(x.head_locked)))?;
                patches.add(node, 4, floats(|x| x.diffuse))?;
                patches.add(node, 5, floats(|x| x.width))?;
                patches.add(node, 6, floats(|x| x.height))?;
                patches.add(node, 7, floats(|x| x.depth))?;
                patches.add(node, 8, floats(|x| x.divergence))?;
                patches.add(node, 9, floats(|x| x.azimuth_range))?;
                patches.add(node, 10, floats(|x| x.position_range))?;
                patches.add(node, 11, blocks.map(|x| flag(x.channel_lock)))?;
                patches.add(
                    node,
                    12,
                    blocks.map(|x| {
                        Value::Distance((x.max_distance_present != 0).then_some(x.max_distance))
                    }),
                )?;
                patches.add(node, 13, blocks.map(|x| flag(x.jump)))?;
                patches.add(
                    node,
                    14,
                    blocks.map(|x| {
                        Value::Samples((x.interpolation_present != 0).then_some(x.interpolation))
                    }),
                )?;
            }
            for bi in 0..bt.ds_blocks.len {
                let blocks = [
                    &b.direct_blocks[bt.ds_blocks.offset + bi],
                    &o.direct_blocks[ot.ds_blocks.offset + bi],
                    &e.direct_blocks[et.ds_blocks.offset + bi],
                ];
                same_time(blocks.map(|x| (x.start, x.end)))?;
                let node = blocks[0].node;
                patches.add(node, 1, blocks.map(|x| Value::Float(x.gain)))?;
                patches.add(node, 3, blocks.map(|x| flag(x.head_locked)))?;
            }
        }
    }
    for pi in 0..b.hoa.len() {
        let (bp, op, ep) = (&b.hoa[pi], &o.hoa[pi], &e.hoa[pi]);
        require(
            baseline.string(bp.object_id) == original.string(op.object_id)
                && original.string(op.object_id) == effective.string(ep.object_id)
                && baseline.string(bp.pack_id) == original.string(op.pack_id)
                && original.string(op.pack_id) == effective.string(ep.pack_id),
            "HOA pack 身份不一致",
        )?;
        same_size(bp.channels.len, op.channels.len, ep.channels.len)?;
        for ci in 0..bp.channels.len {
            let bc = b.hoa_channels[bp.channels.offset + ci];
            let oc = o.hoa_channels[op.channels.offset + ci];
            let ec = e.hoa_channels[ep.channels.offset + ci];
            require(
                baseline.string(bc.uid) == original.string(oc.uid)
                    && original.string(oc.uid) == effective.string(ec.uid),
                "HOA 轨道顺序不一致",
            )?;
            same_size(bc.blocks.len, oc.blocks.len, ec.blocks.len)?;
            for bi in 0..bc.blocks.len {
                let blocks = [
                    &b.hoa_blocks[bc.blocks.offset + bi],
                    &o.hoa_blocks[oc.blocks.offset + bi],
                    &e.hoa_blocks[ec.blocks.offset + bi],
                ];
                same_time(blocks.map(|x| (x.start, x.end)))?;
                patches.add(blocks[0].node, 3, blocks.map(|x| flag(x.head_locked)))?;
            }
        }
    }
    Ok(patches)
}

/// The source AXML `xml` with the scene's semantic edits applied; errors carry no locator.
pub fn patched_axml(scene: &RenderScene, xml: &[u8]) -> Result<String> {
    let adm = |e: mradm_adm::Error| Error::new(e.code, e.message, Context::None);
    let xml =
        std::str::from_utf8(xml).map_err(|_| adm(mradm_adm::Error::xml("AXML 必须使用 UTF-8")))?;
    let rate = scene.info.sample_rate;
    let document = Document::parse(xml).map_err(adm)?;
    let baseline = Snapshot::extract(&document, rate, &[]).map_err(adm)?;
    let original = match &scene.original {
        Some((s, overlay)) => Side {
            s,
            overlay: Some(overlay),
        },
        None => Side {
            s: &scene.snapshot,
            overlay: Some(&scene.overlay),
        },
    };
    let effective = Side {
        s: &scene.snapshot,
        overlay: Some(&scene.overlay),
    };
    let baseline = Side {
        s: &baseline,
        overlay: None,
    };
    let patches = collect(baseline, original, effective)?;
    document.apply_patches(&patches.0, rate).map_err(adm)
}
