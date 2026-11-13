//! `backends`, `layouts`, `formats` and `input-layouts`: the engine's catalog and tables as the
//! C++ commands printed them.
use crate::output::{left, right, stderr, stdout};
use crate::{Facts, Service};
use mradm_engine::catalog::{self, Backend, Platform};
use mradm_engine::tables;
use std::fmt::Write;

fn yes(value: bool) -> &'static str {
    if value { "yes" } else { "no" }
}

/// `RenderService::backend_capabilities`: the Rust backends from the base platform (the SOFA
/// switch), the Apple backend from the island.
fn backend(platform: &Platform, renderer: &str) -> Option<Backend> {
    let base = Platform {
        sofa: platform.sofa,
        ..Platform::default()
    };
    match renderer {
        "ear" => Some(catalog::ear()),
        "saf" => Some(catalog::vbap()),
        "triple-balance" => Some(catalog::triple_balance()),
        "hoa" => Some(catalog::hoa()),
        "saf-binaural" | "binaural" => Some(catalog::binaural(&base)),
        "room-binaural" => Some(catalog::room()),
        "apple" if cfg!(target_os = "macos") => platform.apple.clone(),
        _ => None,
    }
}

fn layout_label(id: &str) -> &str {
    match id {
        "0+5+0" => "5.1",
        "2+5+0" => "5.1.2",
        "wav71" => "7.1",
        "4+5+0" => "5.1.4",
        "4+5+4" => "9.1.4",
        "4+7+0" => "7.1.4",
        "9+10+3" => "22.2",
        other => other,
    }
}

fn join<T: ToString>(items: &[T], separator: &str) -> String {
    items
        .iter()
        .map(ToString::to_string)
        .collect::<Vec<_>>()
        .join(separator)
}

fn capabilities(out: &mut String, caps: &Backend) {
    let _ = writeln!(out, "Backend: {} {}", caps.name, caps.version);
    let _ = writeln!(out, "  Objects:        {}", yes(caps.supports_objects));
    let _ = writeln!(
        out,
        "  DirectSpeakers: {}",
        yes(caps.supports_direct_speakers)
    );
    let _ = writeln!(out, "  HOA:            {}", yes(caps.supports_hoa));
    let _ = writeln!(out, "  ChannelLock:    {}", yes(caps.supports_channel_lock));
    let _ = writeln!(
        out,
        "  Divergence:     {}",
        yes(caps.supports_object_divergence)
    );
    let _ = writeln!(out, "  ScreenRef:      {}", yes(caps.supports_screen_ref));
    let _ = writeln!(out, "  Diffuse:        {}", yes(caps.supports_diffuse));
    if !caps.hrtf_sources.is_empty() {
        let _ = writeln!(out, "  HRTF sources:   {}", caps.hrtf_sources.join(", "));
    }
    for mode in &caps.modes {
        let _ = writeln!(
            out,
            "  Mode: {} ({}){}",
            mode.display_name,
            mode.id,
            if mode.supports_realtime {
                ""
            } else {
                " — offline only"
            }
        );
        if !mode.sample_rates.is_empty() {
            let _ = writeln!(
                out,
                "    Sample rates: {} Hz",
                join(&mode.sample_rates, ", ")
            );
        }
    }
    let visible = |l: &&catalog::Layout| l.is_binaural || l.channel_count != 2;
    let _ = writeln!(
        out,
        "  Layouts ({}):",
        caps.layouts.iter().filter(visible).count()
    );
    for layout in caps.layouts.iter().filter(visible) {
        let mut flags = String::new();
        if layout.channel_count > 0 {
            let _ = write!(flags, "{}ch", layout.channel_count);
        }
        if layout.lfe_count > 0 {
            let _ = write!(flags, " {}lfe", layout.lfe_count);
        }
        if layout.is_3d {
            flags.push_str(" 3d");
        }
        if layout.supports_spread {
            flags.push_str(" spread");
        }
        if layout.is_binaural {
            flags.push_str(" binaural");
        }
        let label = if layout.is_binaural {
            "binaural"
        } else {
            layout_label(&layout.id)
        };
        let _ = writeln!(
            out,
            "    {}  {}  {}",
            left(label, 12, ' '),
            left(&layout.display_name, 30, ' '),
            flags
        );
    }
}

pub(crate) fn backends(service: &impl Service) -> i32 {
    const RENDERERS: &[&str] = if cfg!(target_os = "macos") {
        &[
            "ear",
            "saf",
            "triple-balance",
            "hoa",
            "saf-binaural",
            "apple",
        ]
    } else {
        &["ear", "saf", "triple-balance", "hoa", "saf-binaural"]
    };
    let platform = service.platform(Facts::Render);
    let mut out = String::new();
    let mut first = true;
    for renderer in RENDERERS {
        let Some(caps) = backend(&platform, renderer) else {
            continue;
        };
        if !first {
            out.push('\n');
        }
        first = false;
        capabilities(&mut out, &caps);
    }
    stdout(out.as_bytes());
    0
}

fn lower(text: &str) -> String {
    text.to_ascii_lowercase()
}

fn layout_query(layout: &str) -> String {
    let key = lower(layout);
    match key.as_str() {
        "" => String::new(),
        "binaural" => "binaural".into(),
        "5.1" | "0+5+0" => "5.1".into(),
        "5.1.2" | "2+5+0" => "5.1.2".into(),
        "7.1" | "wav71" | "0+7+0" => "7.1".into(),
        "5.1.4" | "4+5+0" | "atmos514" => "5.1.4".into(),
        "7.1.4" | "4+7+0" | "atmos714" => "7.1.4".into(),
        "9.1.6" | "atmos916" => "9.1.6".into(),
        "22.2" | "9+10+3" | "cicp13" => "22.2".into(),
        "hoa3" => "hoa3".into(),
        _ => key,
    }
}

fn layout_format(format: &str) -> String {
    let key = lower(format);
    match key.as_str() {
        "wave" => "wav".into(),
        "m4a" | "mp4" => "apac".into(),
        _ => key,
    }
}

pub(crate) fn layouts(service: &impl Service, format: &str, layout: &str, renderer: &str) -> i32 {
    let format = layout_format(format);
    let layout = layout_query(layout);
    let renderer = lower(renderer);
    let has_renderer = !renderer.is_empty();
    let heading = if format == "apac" {
        "apac/m4a"
    } else {
        format.as_str()
    };
    let rows = tables::output_layouts(&service.platform(Facts::Document(1)));
    let mut out = String::new();
    let _ = writeln!(out, "Format: {heading}");
    if has_renderer {
        let name = backend(&service.platform(Facts::Render), &renderer).map(|b| b.name);
        let _ = writeln!(out, "Renderer: {}", name.unwrap_or_default());
    }
    let _ = writeln!(
        out,
        "{} {} {} Final channel order",
        left("Layout", 10, ' '),
        left("Channels", 8, ' '),
        left("Container mapping", 46, ' ')
    );
    let _ = writeln!(
        out,
        "{} {} {} {}",
        left("", 10, '-'),
        left("", 8, '-'),
        left("", 46, '-'),
        left("", 19, '-')
    );
    let supported = |row: &tables::LayoutRow| {
        let r = if renderer == "binaural" {
            "saf-binaural"
        } else {
            renderer.as_str()
        };
        row.supported_by.iter().any(|s| s == r)
    };
    let mut any = false;
    for row in &rows {
        if row.format != format
            || (!layout.is_empty() && row.layout != layout)
            || (has_renderer && !supported(row))
        {
            continue;
        }
        any = true;
        let _ = writeln!(
            out,
            "{} {} {} {}",
            left(row.layout, 10, ' '),
            left(&row.channels.to_string(), 8, ' '),
            left(row.container, 46, ' '),
            row.order
        );
        if !row.note.is_empty() {
            let _ = writeln!(
                out,
                "{} {} {} note: {}",
                left("", 10, ' '),
                left("", 8, ' '),
                left("", 46, ' '),
                row.note
            );
        }
    }
    stdout(out.as_bytes());
    if any {
        return 0;
    }
    let message = match (layout.is_empty(), has_renderer) {
        (true, true) => {
            format!("No layout is supported for format '{format}' with renderer '{renderer}'.\n")
        }
        (true, false) => format!("No channel-order table is available for format '{format}'.\n"),
        (false, true) => format!(
            "Layout '{layout}' is not supported for format '{format}' with renderer '{renderer}'.\n"
        ),
        (false, false) => format!("Layout '{layout}' is not supported for format '{format}'.\n"),
    };
    stderr(message.as_bytes());
    1
}

pub(crate) fn formats(service: &impl Service) -> i32 {
    let platform = service.platform(Facts::Document(2));
    let mut out = String::new();
    let _ = writeln!(
        out,
        "Build features: apac={}  iamf={}  iamf_mp4_packager={}  sofa={}  hptf={}\n",
        yes(platform.apac),
        yes(platform.iamf),
        yes(platform.iamf && platform.iamf_mp4_packager),
        yes(platform.sofa),
        yes(true)
    );
    for f in tables::output_formats(&platform) {
        let _ = writeln!(
            out,
            "{} {}",
            left(f.format, 10, ' '),
            f.extensions.join(" ")
        );
        if f.available {
            out.push_str("  available\n");
        } else {
            let _ = writeln!(out, "  unavailable: {}", f.available_reason);
        }
        let _ = writeln!(
            out,
            "  {}  channels: {}  sample rate: {}  height: {}",
            if f.lossy { "lossy" } else { "lossless" },
            if f.max_channels == 0 {
                "any".to_owned()
            } else {
                format!("up to {}", f.max_channels)
            },
            if f.fixed_sample_rate == 0 {
                "any".to_owned()
            } else {
                format!("{} Hz", f.fixed_sample_rate)
            },
            yes(f.supports_height)
        );
        if !f.bit_depths.is_empty() {
            let _ = writeln!(out, "  bit depths: {}", f.bit_depths.join(" "));
        }
        if let Some(b) = &f.bitrate {
            let _ = writeln!(
                out,
                "  bitrate ({}): {}-{} kbps (0 = auto)",
                if b.per_channel {
                    "per channel"
                } else {
                    "total"
                },
                b.min_kbps,
                b.max_kbps
            );
        }
        if !f.note.is_empty() {
            let _ = writeln!(out, "  note: {}", f.note);
        }
        out.push('\n');
    }
    stdout(out.as_bytes());
    0
}

pub(crate) fn input_layouts(format: &str, layout: &str) -> i32 {
    if format == "json" {
        stdout(format!("{}\n", tables::input_layouts_json()).as_bytes());
        return 0;
    }
    let mut out = String::new();
    out.push_str("Coordinate convention: azimuth +left / -right (0 front); elevation +up.\n");
    out.push_str("Aliases: L/FL=M+030 (+30°,0°), R/FR=M-030 (-30°,0°), C/FC=M+000, LFE=LFE1.\n");
    out.push_str("Custom mappings: 1-64 unique labels; U+110/U-110 require @30 or @45.\n\n");
    let mut found = false;
    for preset in mradm_scene::input::input_layouts() {
        if !layout.is_empty() && layout != preset.id && layout != preset.internal_id {
            continue;
        }
        found = true;
        let _ = write!(
            out,
            "{} ({} channels, internal {})",
            preset.id,
            preset.channels.len(),
            preset.internal_id
        );
        if preset.wave_channel_mask != 0 {
            let _ = write!(out, "; WAVE mask 0x{:X}", preset.wave_channel_mask);
        }
        out.push('\n');
        out.push_str("     file order:");
        for channel in &preset.channels {
            let _ = write!(out, " {}", channel.token);
        }
        out.push('\n');
        for (index, channel) in preset.channels.iter().enumerate() {
            let number = right(&(index + 1).to_string(), 2);
            let label = left(channel.speaker_label, 8, ' ');
            if channel.is_lfe {
                let _ = writeln!(out, "  {number}: {label} LFE (no geometric position)");
            } else {
                let _ = writeln!(
                    out,
                    "  {number}: {label} az={}° el={}°",
                    right(&format!("{:.1}", channel.azimuth), 6),
                    right(&format!("{:.1}", channel.elevation), 5)
                );
            }
        }
        out.push('\n');
    }
    stdout(out.as_bytes());
    if !found {
        stderr(
            format!("unknown input layout '{layout}'; use mradm input-layouts without --layout\n")
                .as_bytes(),
        );
        return 1;
    }
    0
}
