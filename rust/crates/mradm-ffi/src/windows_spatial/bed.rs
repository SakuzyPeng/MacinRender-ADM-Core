//! Input channels in project order. The static slots are named by Windows. Dynamic
//! slots take their coordinates from the same speaker geometry the renderer uses.
use mradm_engine::catalog::SpatialLayout;
use mradm_engine::request::{Error, code};
use mradm_render::routing::LFE_SPLIT_POWER_GAIN;
use mradm_scene::layouts::{self, Geometry};
use std::mem::MaybeUninit;
use windows::Win32::Media::Audio::{
    AudioObjectType, AudioObjectType_BackLeft as BL, AudioObjectType_BackRight as BR,
    AudioObjectType_Dynamic as DYNAMIC, AudioObjectType_FrontCenter as FC,
    AudioObjectType_FrontLeft as FL, AudioObjectType_FrontRight as FR,
    AudioObjectType_LowFrequency as LFE, AudioObjectType_None, AudioObjectType_SideLeft as SL,
    AudioObjectType_SideRight as SR, AudioObjectType_TopBackLeft as TBL,
    AudioObjectType_TopBackRight as TBR, AudioObjectType_TopFrontLeft as TFL,
    AudioObjectType_TopFrontRight as TFR,
};

struct Layout {
    id: &'static str,
    name: &'static str,
    channels: &'static [AudioObjectType],
}

#[rustfmt::skip]
const LAYOUTS: &[Layout] = &[
    Layout { id: "4+7+0", name: "7.1.4", channels: &[FL, FR, FC, LFE, SL, SR, BL, BR, TFL, TFR, TBL, TBR] },
    Layout { id: "4+5+0", name: "5.1.4", channels: &[FL, FR, FC, LFE, BL, BR, TFL, TFR, TBL, TBR] },
    Layout { id: "0+5+0", name: "5.1", channels: &[FL, FR, FC, LFE, BL, BR] },
    Layout { id: "9.1.6", name: "9.1.6", channels: &[
        FL, FR, FC, LFE, SL, SR, BL, BR, DYNAMIC, DYNAMIC, TFL, TFR, DYNAMIC, DYNAMIC, TBL, TBR,
    ] },
    Layout { id: "9+10+3", name: "22.2", channels: &[
        DYNAMIC, DYNAMIC, FC, LFE, BL, BR, FL, FR, DYNAMIC, LFE, SL, SR,
        TFL, TFR, DYNAMIC, DYNAMIC, TBL, TBR, DYNAMIC, DYNAMIC, DYNAMIC, DYNAMIC, DYNAMIC, DYNAMIC,
    ] },
];

pub(crate) fn layouts() -> Vec<SpatialLayout> {
    LAYOUTS
        .iter()
        .map(|layout| SpatialLayout {
            id: layout.id.into(),
            display_name: layout.name.into(),
            channel_count: layout.channels.len() as u16,
        })
        .collect()
}

pub(super) struct Bed {
    pub channels: usize,
    pub mask: AudioObjectType,
    pub dynamic: u32,
    pub routes: Vec<Route>,
}

pub(super) struct Route {
    pub kind: AudioObjectType,
    pub position: Option<[f32; 3]>,
    source: usize,
    second_lfe: Option<usize>,
}

impl Bed {
    pub fn new(id: &str, channels: u32, geometry: Geometry) -> Result<Self, Error> {
        let layout = LAYOUTS.iter().find(|l| l.id == id).ok_or_else(|| {
            Error::new(
                code::UNSUPPORTED,
                "系统空间音频不支持该监听布局",
                format!("layout={id}"),
            )
        })?;
        if channels as usize != layout.channels.len() {
            return Err(Error::new(
                code::INVALID_ARGUMENT,
                "监听声道数与布局不符",
                format!("layout={id} channels={channels}"),
            ));
        }
        let speakers = layouts::find(id, geometry)
            .filter(|l| l.speakers.len() == layout.channels.len())
            .ok_or_else(|| {
                Error::new(
                    code::INVALID_ARGUMENT,
                    "Missing system-spatial speaker geometry",
                    "",
                )
            })?;
        let mut bed = Self {
            channels: channels as usize,
            mask: AudioObjectType_None,
            dynamic: 0,
            routes: Vec::with_capacity(channels as usize),
        };
        for (source, (&kind, speaker)) in layout.channels.iter().zip(&speakers.speakers).enumerate()
        {
            // Windows exposes a single non-directional LFE object for both 22.2 LFE inputs.
            if kind == LFE
                && let Some(first) = bed.routes.iter_mut().find(|r| r.kind == LFE)
            {
                first.second_lfe = Some(source);
                continue;
            }
            let position = if kind == DYNAMIC {
                bed.dynamic += 1;
                let azimuth = speaker.azimuth * std::f32::consts::PI / 180.0;
                let elevation = speaker.elevation * std::f32::consts::PI / 180.0;
                Some([
                    -elevation.cos() * azimuth.sin(),
                    elevation.sin(),
                    -elevation.cos() * azimuth.cos(),
                ])
            } else {
                bed.mask |= kind;
                None
            };
            bed.routes.push(Route {
                kind,
                position,
                source,
                second_lfe: None,
            });
        }
        Ok(bed)
    }
}

impl Route {
    // GetBuffer may expose fewer frames than Begin requested. Only those frames
    // contribute to the reference implementation's block-wide LFE activity test.
    pub fn write(&self, pcm: &[f32], channels: usize, output: &mut [MaybeUninit<f32>]) {
        let pcm = &pcm[..output.len() * channels];
        if let Some(second) = self.second_lfe {
            let active = |ch: usize| pcm.chunks_exact(channels).any(|f| f[ch].abs() > 1.0e-12);
            let gain = if active(self.source) && active(second) {
                LFE_SPLIT_POWER_GAIN
            } else {
                1.0
            };
            for (out, frame) in output.iter_mut().zip(pcm.chunks_exact(channels)) {
                out.write((frame[self.source] + frame[second]) * gain);
            }
        } else {
            for (out, frame) in output.iter_mut().zip(pcm.chunks_exact(channels)) {
                out.write(frame[self.source]);
            }
        }
    }
}
