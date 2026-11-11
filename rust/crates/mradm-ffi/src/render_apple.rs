//! Private boundary of the Apple AUSpatialMixer recipe (`mradm-render` `apple`, PCM consistency
//! 5.6 B): the Apple island prepares its buses from the policy-applied Rust scene and reads them
//! back as plain records, which stay valid until `mradm_render_apple_plan_destroy`.
use crate::adm::Text;
use crate::render::{RenderMatrixRoute, boundary, bytes, matrix_routes};
use crate::scene::SceneHandle;
use mradm_render::apple::{self, Bus, Routing, SourceMode};
use mradm_render::routing::{Level, LfeTarget};
use std::ptr;

/// `log` receives the `apple` lines in order (level 1 info, 2 warning), also before an error.
#[repr(C)]
pub struct RenderAppleRequest {
    scene: *const SceneHandle,
    layout_id: Text,
    binaural: u32,
    /// `adm::DirectSpeakersRoutingMode`: 0 automatic, 1 label, 2 position, 3 matrix.
    routing: u32,
    has_matrix: u32,
    matrix: *const RenderMatrixRoute,
    matrix_len: usize,
    lfe_split_power: u32,
    speaker_spread_none: u32,
    binaural_spread_cloud: u32,
    input_path: Text,
    log_context: *mut std::ffi::c_void,
    log: Option<unsafe extern "C" fn(*mut std::ffi::c_void, u32, Text)>,
}

#[repr(C)]
pub struct RenderAppleOutput {
    output: usize,
    gain: f32,
}

/// `lfe_target`: 0 none, 1 LFE1, 2 LFE2. Azimuth is positive to the right.
#[repr(C)]
pub struct RenderAppleEvent {
    start: u64,
    end: u64,
    azimuth: f32,
    elevation: f32,
    distance: f32,
    gain: f32,
    lfe_target: u32,
    head_locked: u32,
    outputs: *const RenderAppleOutput,
    outputs_len: usize,
}

/// `mode`: 0 point source, 1 ambience bed, 2 bypass.
#[repr(C)]
pub struct RenderAppleBus {
    channel: u32,
    mode: u32,
    is_lfe: u32,
    is_direct: u32,
    object_id: Text,
    label_key: Text,
    events: *const RenderAppleEvent,
    events_len: usize,
}

/// The recipe: `routing` is the resolved DirectSpeakers mode (1 label, 2 position, 3 matrix);
/// the input format is the scene's.
#[repr(C)]
pub struct RenderApplePlanView {
    input_channels: u32,
    sample_rate: u32,
    frames: u64,
    routing: u32,
    applies_to_22_2: u32,
    split_power: u32,
    has_lfe1: u32,
    has_lfe2: u32,
    spatial: *const RenderAppleBus,
    spatial_len: usize,
    direct: *const RenderAppleBus,
    direct_len: usize,
    lfe: *const RenderAppleBus,
    lfe_len: usize,
}

/// The plan with the records its view points into.
pub struct RenderApplePlan {
    _plan: apple::Plan,
    _outputs: Vec<Vec<Vec<RenderAppleOutput>>>,
    _events: Vec<Vec<RenderAppleEvent>>,
    _buses: [Vec<RenderAppleBus>; 3],
}

fn records(
    buses: &[Bus],
    outputs: &mut Vec<Vec<Vec<RenderAppleOutput>>>,
    events: &mut Vec<Vec<RenderAppleEvent>>,
) -> Vec<RenderAppleBus> {
    buses
        .iter()
        .map(|bus| {
            let bus_outputs: Vec<Vec<RenderAppleOutput>> = bus
                .events
                .iter()
                .map(|e| {
                    e.outputs
                        .iter()
                        .map(|&(output, gain)| RenderAppleOutput { output, gain })
                        .collect()
                })
                .collect();
            let bus_events: Vec<RenderAppleEvent> = bus
                .events
                .iter()
                .zip(&bus_outputs)
                .map(|(e, o)| RenderAppleEvent {
                    start: e.start,
                    end: e.end,
                    azimuth: e.azimuth,
                    elevation: e.elevation,
                    distance: e.distance,
                    gain: e.gain,
                    lfe_target: match e.lfe {
                        LfeTarget::None => 0,
                        LfeTarget::Lfe1 => 1,
                        LfeTarget::Lfe2 => 2,
                    },
                    head_locked: u32::from(e.head_locked),
                    outputs: o.as_ptr(),
                    outputs_len: o.len(),
                })
                .collect();
            let record = RenderAppleBus {
                channel: u32::from(bus.channel),
                mode: match bus.mode {
                    SourceMode::PointSource => 0,
                    SourceMode::AmbienceBed => 1,
                    SourceMode::Bypass => 2,
                },
                is_lfe: u32::from(bus.is_lfe),
                is_direct: u32::from(bus.is_direct),
                object_id: Text::bytes(&bus.object_id),
                label_key: Text::bytes(&bus.label_key),
                events: bus_events.as_ptr(),
                events_len: bus_events.len(),
            };
            // The vectors' heap buffers do not move when the vectors are moved.
            outputs.push(bus_outputs);
            events.push(bus_events);
            record
        })
        .collect()
}

/// Prepares the Apple recipe. On success `*output` owns the records and `*view` points into them;
/// on failure the error fills the message and context buffers (capacity in, length out).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_apple_prepare(
    request: *const RenderAppleRequest,
    output: *mut *mut RenderApplePlan,
    view: *mut RenderApplePlanView,
    message: *mut u8,
    message_len: *mut usize,
    context: *mut u8,
    context_len: *mut usize,
) -> i32 {
    boundary(message, message_len, context, context_len, || {
        let bad = |text: &str| crate::render::bad(text);
        if request.is_null() || output.is_null() || view.is_null() {
            return Err(bad("无效 Apple 渲染请求"));
        }
        unsafe { *output = ptr::null_mut() };
        let r = unsafe { &*request };
        let Some(scene) = (unsafe { r.scene.as_ref() }) else {
            return Err(bad("无效 Apple 渲染请求"));
        };
        let routing = match r.routing {
            0 => Routing::Automatic,
            1 => Routing::Label,
            2 => Routing::Position,
            3 => Routing::Matrix,
            _ => return Err(bad("无效 DirectSpeakers 路由")),
        };
        let matrix = if r.has_matrix != 0 {
            Some(unsafe { matrix_routes(r.matrix, r.matrix_len)? })
        } else {
            None
        };
        let request = apple::Request {
            scene: scene.scene(),
            layout_id: unsafe { bytes(&r.layout_id)? },
            binaural: r.binaural != 0,
            routing,
            matrix: matrix.as_deref(),
            lfe_split_power: r.lfe_split_power != 0,
            speaker_spread_none: r.speaker_spread_none != 0,
            binaural_spread_cloud: r.binaural_spread_cloud != 0,
            input_path: unsafe { bytes(&r.input_path)? },
        };
        let plan = apple::prepare(&request, &mut |level, text| {
            if let Some(log) = r.log {
                let level = match level {
                    Level::Info => 1,
                    Level::Warning => 2,
                };
                unsafe { log(r.log_context, level, Text::bytes(&text)) };
            }
        })?;
        let mut outputs = Vec::new();
        let mut events = Vec::new();
        let buses = [
            records(&plan.spatial, &mut outputs, &mut events),
            records(&plan.direct, &mut outputs, &mut events),
            records(&plan.lfe, &mut outputs, &mut events),
        ];
        let lfe = plan.lfe_routing;
        let info = &scene.scene().info;
        unsafe {
            *view = RenderApplePlanView {
                input_channels: u32::from(info.channels),
                sample_rate: info.sample_rate,
                frames: info.frames,
                routing: match plan.routing {
                    Routing::Automatic => 0,
                    Routing::Label => 1,
                    Routing::Position => 2,
                    Routing::Matrix => 3,
                },
                applies_to_22_2: u32::from(lfe.applies_to_22_2),
                split_power: u32::from(lfe.split_power),
                has_lfe1: u32::from(lfe.has_lfe1),
                has_lfe2: u32::from(lfe.has_lfe2),
                spatial: buses[0].as_ptr(),
                spatial_len: buses[0].len(),
                direct: buses[1].as_ptr(),
                direct_len: buses[1].len(),
                lfe: buses[2].as_ptr(),
                lfe_len: buses[2].len(),
            };
            *output = Box::into_raw(Box::new(RenderApplePlan {
                _plan: plan,
                _outputs: outputs,
                _events: events,
                _buses: buses,
            }));
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mradm_render_apple_plan_destroy(plan: *mut RenderApplePlan) {
    if !plan.is_null() {
        drop(unsafe { Box::from_raw(plan) });
    }
}
