//! The command-line grammar: the option table of every subcommand, parsed with clap and checked in
//! the order of the CLI11 parser the CLI used before, so that the exit codes stay the same:
//!
//! 1. a value option without its value at the end of the line or with an empty `=` value: 114;
//! 2. `-h` / `--help`: the help text, 0;
//! 3. `--version`, then every option of the subcommand in table order: a value outside its set or
//!    range 105, a value that does not convert 104, a single-valued option given twice 114;
//! 4. no subcommand 106, then in table order a missing required option 106, an option missing the
//!    option it requires 107, two excluded options 108;
//! 5. unexpected arguments: 109.
//!
//! The messages follow CLI11's; the help layout is clap's.
use crate::convert;
use clap::error::{ContextKind, ContextValue, ErrorKind};
use clap::{Arg, ArgAction, ArgMatches, Command};
use std::ffi::OsString;

/// How an option's values are checked and converted.
#[derive(Clone, Copy)]
pub(crate) enum Kind {
    /// Free text.
    Text,
    /// One of the listed values (`CLI::IsMember`).
    Member(&'static [&'static str]),
    /// One of the listed values with a custom message (the renderer validators).
    Choice {
        values: &'static [&'static str],
        /// The validator's description (its list in the missing-value message).
        described: &'static [&'static str],
        message: &'static str,
    },
    /// An unsigned integer in an inclusive range (`CLI::Range`).
    Range(u32, u32),
    /// An unsigned integer.
    Unsigned,
    /// A finite binary32 decimal in an inclusive range.
    Float32(f32, f32),
    /// A finite binary64 decimal in an inclusive range.
    Float64(f64, f64),
    /// A flag, optionally `--name=value`.
    Flag,
    /// The APAC DRC flag pair: `--apac-drc-none` stores false, `--apac-drc-music` true.
    Drc,
    /// A boolean value.
    Boolean,
}

/// One option of a subcommand's table.
pub(crate) struct Opt {
    /// The CLI11 name: the first long name, or the positional's name.
    pub(crate) name: &'static str,
    pub(crate) short: Option<char>,
    /// Long names after the first (aliases).
    pub(crate) aliases: &'static [&'static str],
    pub(crate) positional: bool,
    pub(crate) kind: Kind,
    pub(crate) help: &'static str,
    pub(crate) required: bool,
    pub(crate) needs: Option<&'static str>,
    pub(crate) excludes: Option<&'static str>,
}

const fn opt(name: &'static str, kind: Kind, help: &'static str) -> Opt {
    Opt {
        name,
        short: None,
        aliases: &[],
        positional: false,
        kind,
        help,
        required: false,
        needs: None,
        excludes: None,
    }
}

impl Opt {
    const fn short(mut self, c: char) -> Self {
        self.short = Some(c);
        self
    }
    const fn required(mut self) -> Self {
        self.required = true;
        self
    }
    const fn positional(mut self) -> Self {
        self.positional = true;
        self
    }
    const fn alias(mut self, aliases: &'static [&'static str]) -> Self {
        self.aliases = aliases;
        self
    }
    const fn needs(mut self, other: &'static str) -> Self {
        self.needs = Some(other);
        self
    }
    const fn excludes(mut self, other: &'static str) -> Self {
        self.excludes = Some(other);
        self
    }

    /// The long name without dashes (also the clap id).
    fn long(&self) -> &'static str {
        self.name.trim_start_matches('-')
    }

    fn takes_value(&self) -> bool {
        !matches!(self.kind, Kind::Flag | Kind::Drc)
    }

    /// The value's name in the help text.
    fn value_name(&self) -> &'static str {
        match self.kind {
            Kind::Range(..) | Kind::Unsigned => "UINT",
            Kind::Float32(..) | Kind::Float64(..) => "FLOAT",
            Kind::Boolean => "BOOLEAN",
            _ => "TEXT",
        }
    }

    /// The type CLI11 named in its missing-value message.
    fn type_name(&self) -> String {
        match self.kind {
            Kind::Text => "TEXT".into(),
            Kind::Member(values) => format!("TEXT:{{{}}}", values.join(",")),
            Kind::Choice { described, .. } => format!("TEXT:{}", described.join(",")),
            Kind::Range(min, max) => format!("UINT:UINT in [{min} - {max}]"),
            Kind::Unsigned => "UINT".into(),
            Kind::Float32(..) | Kind::Float64(..) => "FLOAT".into(),
            Kind::Boolean => "BOOLEAN".into(),
            Kind::Flag | Kind::Drc => String::new(),
        }
    }
}

/// One subcommand.
pub(crate) struct Sub {
    pub(crate) name: &'static str,
    pub(crate) about: &'static str,
    pub(crate) hidden: bool,
    pub(crate) options: Vec<Opt>,
}

const RENDERERS: &[&str] = &[
    "auto",
    "ear",
    "saf",
    "triple-balance",
    "hoa",
    "saf-binaural",
    "room-binaural",
    "apple",
];

#[cfg(target_os = "macos")]
const LAYOUT_RENDERERS: &[&str] = &[
    "ear",
    "saf",
    "triple-balance",
    "hoa",
    "saf-binaural",
    "binaural",
    "apple",
];
#[cfg(not(target_os = "macos"))]
const LAYOUT_RENDERERS: &[&str] = &[
    "ear",
    "saf",
    "triple-balance",
    "hoa",
    "saf-binaural",
    "binaural",
];
#[cfg(target_os = "macos")]
const LAYOUT_RENDERER_VALUES: &[&str] = &[
    "ear",
    "saf",
    "triple-balance",
    "hoa",
    "saf-binaural",
    "apple",
];
#[cfg(not(target_os = "macos"))]
const LAYOUT_RENDERER_VALUES: &[&str] = &["ear", "saf", "triple-balance", "hoa", "saf-binaural"];
#[cfg(target_os = "macos")]
const LAYOUT_RENDERER_MESSAGE: &str =
    "expected one of: ear, saf, triple-balance, hoa, saf-binaural, apple";
#[cfg(not(target_os = "macos"))]
const LAYOUT_RENDERER_MESSAGE: &str =
    "expected one of: ear, saf, triple-balance, hoa, saf-binaural";
#[cfg(target_os = "macos")]
const LAYOUT_RENDERER_HELP: &str =
    "Optional renderer filter: ear, saf, triple-balance, hoa, saf-binaural, apple";
#[cfg(not(target_os = "macos"))]
const LAYOUT_RENDERER_HELP: &str =
    "Optional renderer filter: ear, saf, triple-balance, hoa, saf-binaural";

fn render() -> Sub {
    use Kind::*;
    let options = vec![
        opt(
            "--input",
            Text,
            "Input ADM BWF or channel-based WAV/RF64/BW64 path",
        )
        .short('i')
        .required(),
        opt("--output", Text, "Output audio path").short('o'),
        opt(
            "--input-layout",
            Member(&[
                "auto", "5.1", "5.1.2", "7.1", "5.1.4", "7.1.4", "9.1.4", "9.1.6", "22.2",
            ]),
            "Ordinary input layout: auto, 5.1, 5.1.2, 7.1, 5.1.4, 7.1.4, 9.1.4, 9.1.6, 22.2",
        )
        .excludes("--input-channels"),
        opt(
            "--input-channels",
            Text,
            "Custom labels in file order: L/FL=+30deg left, R/FR=-30deg right, C/FC=0deg front, \
             LFE=LFE1; azimuth is +left/-right; see mradm input-layouts",
        )
        .excludes("--input-layout"),
        opt(
            "--output-layout",
            Text,
            "Output semantic/layout: binaural (default) or a multichannel/HOA layout; spatial WAV \
             writes a WAVE mask or ADM AXML/CHNA; use 'mradm layouts --format <fmt>' for final \
             container channel order",
        ),
        opt(
            "--renderer",
            Choice {
                values: RENDERERS,
                described: RENDERERS,
                message: "expected one of: auto, ear, saf, triple-balance, hoa, saf-binaural, \
                          room-binaural, apple",
            },
            "Renderer backend: auto, ear, saf, triple-balance, hoa, saf-binaural, room-binaural, apple",
        ),
        opt("--no-peak-limit", Flag, "Disable True Peak limiting"),
        opt(
            "--peak-limit-dbtp",
            Float32(-60.0, 0.0),
            "True Peak target in dBTP",
        ),
        opt(
            "--peak-normalize-to-limit",
            Flag,
            "Raise global gain up to --peak-limit-dbtp when measured True Peak is below the ceiling",
        ),
        opt(
            "--final-gain-db",
            Float64(f64::MIN, f64::MAX),
            "Unconstrained final gain in dB, applied after loudness/peak staging; bypasses peak \
             limiting and may exceed 0 dBFS (default: 0)",
        ),
        opt(
            "--output-bit-depth",
            Member(&["f32", "i24", "i16"]),
            "Output bit depth: f32, i24, i16; use i24 for PCM BW64 ADM delivery",
        ),
        opt(
            "--loudness-target",
            Float32(-70.0, 0.0),
            "Normalise integrated loudness to this LUFS target (enables loudness normalisation)",
        ),
        opt(
            "--start",
            Float64(0.0, f64::MAX),
            "Trim output to start at this time in seconds (default: 0 = from the beginning)",
        ),
        opt(
            "--end",
            Float64(f64::MIN, f64::MAX),
            "Trim output to end at this absolute time in seconds (default: render to the end); \
             must be > --start. Loudness/True-Peak are measured over the trimmed segment",
        ),
        opt(
            "--interp-ms",
            Range(0, 500),
            "Default gain-interpolation ramp in ms when ADM block has no \
             jumpPosition/interpolationLength (default: 5, set to 0 for instant switching)",
        ),
        opt(
            "--object-smoothing-frames",
            Range(0, 48000),
            "De-zipper window for dense Objects metadata updates in sample frames (default: 0 for \
             sample-accurate block switching; currently ignored by the apple backend)",
        ),
        opt(
            "--opus-bitrate-per-ch",
            Range(6, 320),
            "Opus MKA VBR target bitrate per channel in kbps (6-320); omit for auto: 64 kbps/ch, \
             128 kbps floor for one- or two-channel output",
        ),
        opt(
            "--apac-bitrate",
            Range(64, 32768),
            "APAC total target/hint bitrate in kbps (64-32768); actual measured bitrate may differ \
             substantially; omit for layout default",
        ),
        opt(
            "--apac-drc-none",
            Drc,
            "APAC DRC profile: --apac-drc-music (default) or --apac-drc-none",
        )
        .alias(&["--apac-drc-music"]),
        opt(
            "--apac-container",
            Member(&["mpeg4", "caf"]),
            "APAC output container: mpeg4 (.m4a/.mp4, default) or caf (.caf)",
        ),
        opt("--sofa", Text, "User SOFA HRIR file for binaural rendering"),
        opt("--room", Text, "Room package room.json (room-binaural)"),
        opt(
            "--room-quality",
            Member(&["exact", "realtime"]),
            "Room processing quality: exact (default), realtime",
        )
        .needs("--room"),
        opt(
            "--room-hrir",
            Text,
            "Personal SOFA, HRIR or distance collection directory/index (room-binaural)",
        ),
        opt(
            "--screen-config",
            Text,
            "Reference/reproduction screen JSON (room-binaural)",
        ),
        opt(
            "--room-metres-per-unit",
            Float64(0.01, 100.0),
            "Explicit fixed room scale in m/unit; omitted uses automatic room mapping",
        ),
        opt("--semantic-policy", Text, "ADM semantic render policy JSON"),
        opt(
            "--write-semantic-report",
            Text,
            "Write effective ADM semantic report JSON after applying policy",
        ),
        opt(
            "--speaker-geometry",
            Member(&["standard", "apple"]),
            "Output speaker coordinates for EAR/SAF: standard (existing project/ADM geometry) or \
             apple (CoreAudio fixed geometry); Apple renderer always uses apple",
        ),
        opt(
            "--direct-speakers-routing",
            Member(&["auto", "label", "position", "matrix"]),
            "DirectSpeakers routing: auto (label for SAF/Apple speakers, position for Apple \
             binaural), label (one-hot on match, otherwise spatialize label/nominal direction), or \
             position (spatialize nominal coordinates), or matrix (user label-to-label routing)",
        ),
        opt(
            "--direct-speakers-matrix",
            Text,
            "DirectSpeakers sparse matrix JSON (required with --direct-speakers-routing matrix)",
        ),
        opt(
            "--speaker-spread-mode",
            Member(&["auto", "none", "mdap"]),
            "Speaker Objects extent: auto (Triple Balance size, otherwise MDAP for 3D), none \
             (ignore extent), mdap",
        ),
        opt(
            "--triple-balance-mode",
            Member(&["standard", "d"]),
            "Triple Balance mode: standard, or d (coherent size; offline 48 kHz / 9.1.6)",
        ),
        opt(
            "--binaural-spread-mode",
            Member(&["auto", "none", "cloud", "saf-spreader"]),
            "Binaural Objects extent spread algorithm: auto (cloud), none, cloud, saf-spreader \
             [experimental: SAF covariance-matching STFT-domain spreader]",
        ),
        opt(
            "--lfe-routing",
            Member(&["direct", "split-power"]),
            "22.2 LFE routing: direct (LFE1->ch3, LFE2->ch9) or split-power (one LFE to both at \
             -3.0103 dB; native dual LFE is rejected)",
        ),
        opt(
            "--apple-spatial-preset",
            Member(&["off", "headphone-default", "headphone-movie"]),
            "Apple binaural AUSpatialMixer factory preset: off, headphone-default, headphone-movie",
        ),
        opt(
            "--apple-speaker-rendering-flags",
            Flag,
            "Enable Apple speaker InterAuralDelay and DistanceAttenuation rendering flags (legacy \
             behavior; default: disabled for ADM/SAF-style gains)",
        ),
        opt(
            "--listener-yaw",
            Float32(-180.0, 180.0),
            "Listener head yaw in degrees, +left (binaural backends: Apple macOS / SAF)",
        ),
        opt(
            "--listener-pitch",
            Float32(-90.0, 90.0),
            "Listener head pitch in degrees, +up (binaural backends: Apple macOS / SAF)",
        ),
        opt(
            "--listener-roll",
            Float32(-180.0, 180.0),
            "Listener head roll in degrees (binaural backends: Apple macOS / SAF)",
        ),
        opt(
            "--iamf-container",
            Member(&["obu", "mp4"]),
            "IAMF output container [requires MR_ADM_ENABLE_IAMF build]: obu (raw .iamf stream, \
             default) or mp4 (ISOBMFF via mp4box/ffmpeg)",
        ),
        opt(
            "--iamf-layers",
            Text,
            "IAMF scalable channel layers, comma-separated; empty = single output layout",
        ),
        opt("--verbose", Flag, "Enable verbose logs").short('v'),
    ];
    Sub {
        name: "render",
        about: "Render an ADM BWF or ordinary channel-based WAVE file",
        hidden: false,
        options,
    }
}

fn subcommands() -> Vec<Sub> {
    use Kind::*;
    vec![
        render(),
        Sub {
            name: "inspect",
            about: "Print ADM or auto-detected channel-bed scene metadata",
            hidden: false,
            options: vec![
                opt("file", Text, "ADM BWF or channel-based WAVE input path")
                    .positional()
                    .required(),
                opt(
                    "--xml",
                    Flag,
                    "Dump raw AXML chunk instead of parsed summary",
                ),
                opt(
                    "--write-semantic-policy-template",
                    Text,
                    "Write editable ADM semantic policy JSON template for this input",
                ),
            ],
        },
        Sub {
            name: "backends",
            about: "List available renderer backends and supported layouts",
            hidden: false,
            options: vec![],
        },
        Sub {
            name: "layouts",
            about: "Show final channel order for an output format, optionally filtered by renderer",
            hidden: false,
            options: vec![
                opt(
                    "--format",
                    Member(&["wav", "wave", "caf", "flac", "apac", "m4a", "mp4", "iamf"]),
                    "Output format: wav, caf, flac, apac/m4a/mp4, iamf",
                )
                .required(),
                opt(
                    "--layout",
                    Text,
                    "Optional layout filter, e.g. 7.1, 9.1.6, 22.2, binaural",
                ),
                opt(
                    "--renderer",
                    Choice {
                        values: LAYOUT_RENDERERS,
                        described: LAYOUT_RENDERER_VALUES,
                        message: LAYOUT_RENDERER_MESSAGE,
                    },
                    LAYOUT_RENDERER_HELP,
                ),
            ],
        },
        Sub {
            name: "input-layouts",
            about: "Show ordinary multichannel input layouts and label geometry",
            hidden: false,
            options: vec![
                opt(
                    "--format",
                    Member(&["text", "json"]),
                    "Output format: text or json",
                ),
                opt(
                    "--layout",
                    Text,
                    "Optional preset filter, e.g. 5.1, 7.1.4, 22.2",
                ),
            ],
        },
        Sub {
            name: "formats",
            about: "List output container formats with availability and constraints",
            hidden: false,
            options: vec![],
        },
        Sub {
            name: "export",
            about: "Write a new ADM BWF with semantic-policy overrides applied (source PCM reused)",
            hidden: false,
            options: vec![
                opt("--input", Text, "ADM BWF/WAV input path")
                    .short('i')
                    .required(),
                opt("--output", Text, "Output ADM BWF path")
                    .short('o')
                    .required(),
                opt(
                    "--semantic-policy",
                    Text,
                    "Semantic policy JSON to apply before writing (optional; omit for a plain \
                     round-trip)",
                ),
            ],
        },
        Sub {
            name: "__apac-encode",
            about: "(internal) APAC encode worker with heartbeat protocol",
            hidden: true,
            options: vec![
                opt("--in", Text, "input float32 WAV (48 kHz)").required(),
                opt("--out", Text, "output APAC path").required(),
                opt("--layout", Text, "layout id").required(),
                opt(
                    "--bitrate",
                    Unsigned,
                    "total target bitrate kbps (0 = layout default)",
                ),
                opt("--drc", Boolean, "DRC profile: 1 = Music, 0 = None"),
                opt("--container", Text, "container: mpeg4 | caf"),
            ],
        },
        Sub {
            name: "room-check",
            about: "Validate and prepare a room package and its personal HRIR",
            hidden: false,
            options: vec![
                opt("manifest", Text, "room.json").positional().required(),
                opt(
                    "--hrir",
                    Text,
                    "Personal SOFA, HRIR manifest or distance collection directory",
                )
                .alias(&["--sofa"]),
                opt(
                    "--quality",
                    Member(&["exact", "realtime"]),
                    "Room processing quality: exact (default), realtime",
                ),
            ],
        },
    ]
}

/// The clap command of the table.
fn command(subs: &[Sub]) -> Command {
    let mut root = Command::new("mradm")
        .about("MacinRender ADM Core command-line interface")
        .disable_version_flag(true)
        .disable_help_subcommand(true)
        .arg(
            Arg::new("version")
                .long("version")
                .help("Display program version information and exit")
                .num_args(0..=1)
                .require_equals(true)
                .default_missing_value("")
                .action(ArgAction::Append)
                .value_parser(clap::value_parser!(OsString)),
        );
    for sub in subs {
        let mut command = Command::new(sub.name).about(sub.about).hide(sub.hidden);
        let mut index = 1;
        for o in &sub.options {
            let mut arg = Arg::new(o.long())
                .help(o.help)
                .action(ArgAction::Append)
                .value_parser(clap::value_parser!(OsString));
            if o.positional {
                arg = arg
                    .index(index)
                    .num_args(1)
                    .action(ArgAction::Set)
                    .value_name(o.name);
                index += 1;
            } else {
                arg = arg.long(o.long());
                if let Some(c) = o.short {
                    arg = arg.short(c);
                }
                if o.takes_value() {
                    arg = arg
                        .num_args(1)
                        .allow_hyphen_values(true)
                        .value_name(o.value_name());
                    for alias in o.aliases {
                        arg = arg.visible_alias(alias.trim_start_matches('-'));
                    }
                } else {
                    arg = arg
                        .num_args(0..=1)
                        .require_equals(true)
                        .default_missing_value("");
                    if matches!(o.kind, Kind::Drc) {
                        // `--apac-drc-music` is the same CLI11 option with the opposite default.
                        command = command.arg(
                            Arg::new("apac-drc-music")
                                .long("apac-drc-music")
                                .help("APAC DRC profile: Music (default)")
                                .num_args(0..=1)
                                .require_equals(true)
                                .default_missing_value("")
                                .action(ArgAction::Append)
                                .value_parser(clap::value_parser!(OsString)),
                        );
                    }
                }
            }
            command = command.arg(arg);
        }
        root = root.subcommand(command);
    }
    root
}

/// A parse that ends the program: the text for stdout (help, version) or stderr, and the code.
pub(crate) struct Exit {
    pub(crate) code: i32,
    pub(crate) stdout: String,
    pub(crate) stderr: String,
}

impl Exit {
    fn error(code: i32, message: impl Into<String>) -> Self {
        Self {
            code,
            stdout: String::new(),
            stderr: format!(
                "{}\nRun with --help for more information.\n",
                message.into()
            ),
        }
    }
}

/// An option's values in command-line order.
pub(crate) struct Values<'a> {
    sub: &'a Sub,
    matches: &'a ArgMatches,
}

fn bytes(value: &std::ffi::OsStr) -> Vec<u8> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        value.as_bytes().to_vec()
    }
    #[cfg(not(unix))]
    {
        value.to_string_lossy().into_owned().into_bytes()
    }
}

impl Values<'_> {
    /// The raw values of clap argument `id` with their positions.
    fn raw(&self, id: &str) -> Vec<(usize, Vec<u8>)> {
        let (Some(values), Some(indices)) = (
            self.matches.get_many::<OsString>(id),
            self.matches.indices_of(id),
        ) else {
            return Vec::new();
        };
        indices.zip(values.map(|v| bytes(v))).collect()
    }

    fn option(&self, name: &str) -> &Opt {
        self.sub
            .options
            .iter()
            .find(|o| o.name == name)
            .expect("an option of the table")
    }

    /// The stored texts of option `name` in order (a flag's as CLI11 stored them).
    fn stored(&self, name: &str) -> Vec<Vec<u8>> {
        let o = self.option(name);
        match o.kind {
            Kind::Drc => {
                let mut all: Vec<(usize, Vec<u8>)> = self
                    .raw(o.long())
                    .into_iter()
                    .map(|(i, v)| (i, convert::flag_text(&v, true)))
                    .chain(
                        self.raw("apac-drc-music")
                            .into_iter()
                            .map(|(i, v)| (i, convert::flag_text(&v, false))),
                    )
                    .collect();
                all.sort_by_key(|(i, _)| *i);
                all.into_iter().map(|(_, v)| v).collect()
            }
            Kind::Flag => self
                .raw(o.long())
                .into_iter()
                .map(|(_, v)| convert::flag_text(&v, false))
                .collect(),
            _ => self.raw(o.long()).into_iter().map(|(_, v)| v).collect(),
        }
    }

    pub(crate) fn count(&self, name: &str) -> usize {
        self.stored(name).len()
    }

    /// The single value of a checked option.
    pub(crate) fn text(&self, name: &str) -> Option<Vec<u8>> {
        self.stored(name).into_iter().next()
    }

    pub(crate) fn text_or(&self, name: &str, default: &str) -> Vec<u8> {
        self.text(name)
            .unwrap_or_else(|| default.as_bytes().to_vec())
    }

    /// A checked flag: its last stored value, or `default` when absent.
    pub(crate) fn flag(&self, name: &str, default: bool) -> bool {
        self.stored(name)
            .last()
            .map_or(default, |v| convert::boolean(v).unwrap_or(default))
    }

    pub(crate) fn uint(&self, name: &str) -> Option<u32> {
        self.text(name).and_then(|v| convert::uint32(&v))
    }

    pub(crate) fn f32(&self, name: &str) -> Option<f32> {
        self.text(name)
            .and_then(|v| String::from_utf8(v).ok())
            .and_then(|v| mradm_dsp::decimal::parse_f32(&v))
    }

    pub(crate) fn f64(&self, name: &str) -> Option<f64> {
        self.text(name)
            .and_then(|v| String::from_utf8(v).ok())
            .and_then(|v| mradm_dsp::decimal::parse_f64(&v))
    }
}

/// The parsed command line: the subcommand and its checked values.
pub(crate) struct Parsed {
    subs: Vec<Sub>,
    matches: ArgMatches,
}

impl Parsed {
    /// The subcommand's name and values.
    pub(crate) fn command(&self) -> (&'static str, Values<'_>) {
        let (name, matches) = self.matches.subcommand().expect("a checked subcommand");
        let sub = self
            .subs
            .iter()
            .find(|s| s.name == name)
            .expect("a known subcommand");
        (sub.name, Values { sub, matches })
    }
}

/// `{}` of a C++ double or float as fmt printed a range bound.
fn bound(value: f64) -> String {
    if value.abs() >= 1e16 {
        let text = format!("{value:e}");
        match text.split_once('e') {
            Some((mantissa, exponent)) if !exponent.starts_with('-') => {
                format!("{mantissa}e+{exponent}")
            }
            _ => text,
        }
    } else {
        format!("{value}")
    }
}

fn shown(value: &[u8]) -> String {
    String::from_utf8_lossy(value).into_owned()
}

/// The callback checks of one option (phase 3).
fn check_option(o: &Opt, stored: &[Vec<u8>]) -> Result<(), Exit> {
    if stored.is_empty() {
        return Ok(());
    }
    let name = o.name;
    let many = || {
        Exit::error(
            114,
            format!("{name}: At Most 1 required but received {}", stored.len()),
        )
    };
    let unconverted = || {
        let joined: Vec<String> = stored.iter().map(|v| shown(v)).collect();
        Exit::error(
            104,
            format!("Could not convert: {name} = {}", joined.join(",")),
        )
    };
    match o.kind {
        Kind::Text => {
            if stored.len() > 1 {
                return Err(many());
            }
        }
        Kind::Member(values) => {
            for v in stored {
                if !values.iter().any(|m| m.as_bytes() == &v[..]) {
                    return Err(Exit::error(
                        105,
                        format!("{name}: {} not in {{{}}}", shown(v), values.join(",")),
                    ));
                }
            }
            if stored.len() > 1 {
                return Err(many());
            }
        }
        Kind::Choice {
            values, message, ..
        } => {
            for v in stored {
                if !values.iter().any(|m| m.as_bytes() == &v[..]) {
                    return Err(Exit::error(105, format!("{name}: {message}")));
                }
            }
            if stored.len() > 1 {
                return Err(many());
            }
        }
        Kind::Range(min, max) => {
            for v in stored {
                if !convert::uint32(v).is_some_and(|x| (min..=max).contains(&x)) {
                    return Err(Exit::error(
                        105,
                        format!("{name}: Value {} not in range [{min} - {max}]", shown(v)),
                    ));
                }
            }
            if stored.len() > 1 {
                return Err(many());
            }
        }
        Kind::Unsigned => {
            if stored.len() > 1 {
                return Err(many());
            }
            if convert::uint32(&stored[0]).is_none() {
                return Err(unconverted());
            }
        }
        Kind::Boolean => {
            if stored.len() > 1 {
                return Err(many());
            }
            if convert::boolean(&stored[0]).is_none() {
                return Err(unconverted());
            }
        }
        Kind::Float32(..) | Kind::Float64(..) => {
            if stored.len() > 1 {
                return Err(many());
            }
            let text = &stored[0];
            let decimal = std::str::from_utf8(text).ok();
            let (value, min, max) = match o.kind {
                Kind::Float32(min, max) => (
                    decimal
                        .and_then(mradm_dsp::decimal::parse_f32)
                        .map(f64::from),
                    f64::from(min),
                    f64::from(max),
                ),
                Kind::Float64(min, max) => {
                    (decimal.and_then(mradm_dsp::decimal::parse_f64), min, max)
                }
                _ => unreachable!(),
            };
            let Some(value) = value else {
                return Err(Exit::error(
                    105,
                    format!("{name}: '{}' is not a finite decimal number", shown(text)),
                ));
            };
            if value < min || value > max {
                let (min, max) = match o.kind {
                    // A float bound prints as the float fmt shows.
                    Kind::Float32(min, max) => (bound32(min), bound32(max)),
                    _ => (bound(min), bound(max)),
                };
                return Err(Exit::error(
                    105,
                    format!("{name}: {} is not in range [{min} - {max}]", shown(text)),
                ));
            }
        }
        Kind::Flag | Kind::Drc => {
            if convert::boolean(stored.last().expect("a value")).is_none() {
                return Err(unconverted());
            }
        }
    }
    Ok(())
}

fn bound32(value: f32) -> String {
    if value.abs() >= 1e16 {
        bound(f64::from(value))
    } else {
        format!("{value}")
    }
}

fn os(bytes: Vec<u8>) -> OsString {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStringExt;
        OsString::from_vec(bytes)
    }
    #[cfg(not(unix))]
    {
        OsString::from(String::from_utf8_lossy(&bytes).into_owned())
    }
}

/// Phase 1: a value option at the end of the line, or with an empty `=` value, ends the parse.
/// The line is returned with `-x=value` of a short value option split into `-x` and `=value`,
/// which CLI11 took as the value (clap would drop the `=`).
fn prescan(args: Vec<OsString>, subs: &[Sub]) -> Result<Vec<OsString>, Exit> {
    let tokens: Vec<Vec<u8>> = args.iter().map(|a| bytes(a)).collect();
    let mut i = 1;
    let mut sub = None;
    while i < tokens.len() {
        let t = &tokens[i];
        i += 1;
        if t == b"--" {
            return Ok(args);
        }
        if !t.starts_with(b"-") {
            sub = subs.iter().find(|s| s.name.as_bytes() == &t[..]);
            if sub.is_some() {
                break;
            }
        }
    }
    let Some(sub) = sub else {
        return Ok(args);
    };
    let find_long = |name: &[u8]| {
        sub.options.iter().find(|o| {
            !o.positional
                && (o.long().as_bytes() == name
                    || o.aliases
                        .iter()
                        .any(|a| a.trim_start_matches('-').as_bytes() == name))
        })
    };
    let find_short = |c: u8| {
        sub.options
            .iter()
            .find(|o| o.short == Some(c as char) && o.takes_value())
    };
    let missing = |o: &Opt| {
        Exit::error(
            114,
            format!("{}: 1 required {} missing", o.name, o.type_name()),
        )
    };
    let mut out: Vec<OsString> = tokens[..i].iter().cloned().map(os).collect();
    while i < tokens.len() {
        let t = &tokens[i];
        i += 1;
        if t == b"--" {
            out.extend(tokens[i - 1..].iter().cloned().map(os));
            return Ok(out);
        }
        if let Some(long) = t.strip_prefix(b"--") {
            let (name, value) = match long.iter().position(|&b| b == b'=') {
                Some(p) => (&long[..p], Some(&long[p + 1..])),
                None => (long, None),
            };
            out.push(os(t.clone()));
            if let Some(o) = find_long(name).filter(|o| o.takes_value()) {
                match value {
                    Some([]) => return Err(missing(o)),
                    Some(_) => {}
                    None if i >= tokens.len() => return Err(missing(o)),
                    None => {
                        out.push(os(tokens[i].clone()));
                        i += 1;
                    }
                }
            }
        } else if t.len() >= 2 && t[0] == b'-' && find_short(t[1]).is_some() {
            let o = find_short(t[1]).expect("a short value option");
            if t.len() == 2 {
                if i >= tokens.len() {
                    return Err(missing(o));
                }
                out.push(os(t.clone()));
                out.push(os(tokens[i].clone()));
                i += 1;
            } else if t[2] == b'=' {
                out.push(os(t[..2].to_vec()));
                out.push(os(t[2..].to_vec()));
            } else {
                out.push(os(t.clone()));
            }
        } else {
            out.push(os(t.clone()));
        }
    }
    Ok(out)
}

/// The invalid argument of an unexpected-argument error.
fn invalid_argument(error: &clap::Error) -> Option<String> {
    for kind in [ContextKind::InvalidArg, ContextKind::InvalidSubcommand] {
        if let Some(ContextValue::String(s)) = error.get(kind) {
            return Some(s.clone());
        }
    }
    None
}

/// Parses `args` (with the program name first); `Err` ends the program.
pub(crate) fn parse(args: Vec<OsString>, version: &str) -> Result<Parsed, Exit> {
    let subs = subcommands();
    let mut args = prescan(args, &subs)?;
    let mut extras: Vec<String> = Vec::new();
    let matches = loop {
        match command(&subs).try_get_matches_from(args.iter()) {
            Ok(matches) => break matches,
            Err(e) if matches!(e.kind(), ErrorKind::DisplayHelp) => {
                return Err(Exit {
                    code: 0,
                    stdout: e.render().to_string(),
                    stderr: String::new(),
                });
            }
            Err(e)
                if matches!(
                    e.kind(),
                    ErrorKind::UnknownArgument | ErrorKind::InvalidSubcommand
                ) =>
            {
                let Some(invalid) = invalid_argument(&e) else {
                    return Err(Exit::error(109, e.to_string()));
                };
                let equals = format!("{invalid}=");
                let position = args.iter().skip(1).position(|a| {
                    let a = a.to_string_lossy();
                    a == invalid || a.starts_with(&equals)
                });
                let position = position.or_else(|| {
                    args.iter()
                        .skip(1)
                        .position(|a| a.to_string_lossy().starts_with(&invalid))
                });
                let Some(position) = position else {
                    return Err(Exit::error(109, e.to_string()));
                };
                let removed = args.remove(position + 1);
                extras.push(removed.to_string_lossy().into_owned());
            }
            Err(e) if matches!(e.kind(), ErrorKind::InvalidValue) => {
                return Err(Exit::error(114, e.to_string().trim_end().to_owned()));
            }
            Err(e) => return Err(Exit::error(109, e.to_string().trim_end().to_owned())),
        }
    };
    // Phase 3: `--version`, then the subcommand's options in table order.
    if let Some(values) = matches.get_many::<OsString>("version") {
        let stored: Vec<Vec<u8>> = values
            .map(|v| convert::flag_text(&bytes(v), false))
            .collect();
        if let Some(last) = stored.last() {
            match convert::boolean(last) {
                Some(true) => {
                    return Err(Exit {
                        code: 0,
                        stdout: format!("mradm {version}\n"),
                        stderr: String::new(),
                    });
                }
                Some(false) => {}
                None => {
                    let joined: Vec<String> = stored.iter().map(|v| shown(v)).collect();
                    return Err(Exit::error(
                        104,
                        format!("Could not convert: --version = {}", joined.join(",")),
                    ));
                }
            }
        }
    }
    let parsed = Parsed { subs, matches };
    if parsed.matches.subcommand().is_some() {
        let (_, values) = parsed.command();
        for o in &values.sub.options {
            check_option(o, &values.stored(o.name))?;
        }
    }
    // Phase 4: requirements.
    if parsed.matches.subcommand().is_none() {
        return Err(Exit::error(106, "A subcommand is required"));
    }
    {
        let (_, values) = parsed.command();
        for o in &values.sub.options {
            let count = values.count(o.name);
            if o.required && count == 0 {
                return Err(Exit::error(106, format!("{} is required", o.name)));
            }
            if let Some(other) = o.needs
                && count > 0
                && values.count(other) == 0
            {
                return Err(Exit::error(107, format!("{} requires {other}", o.name)));
            }
            if let Some(other) = o.excludes
                && count > 0
                && values.count(other) != 0
            {
                return Err(Exit::error(108, format!("{} excludes {other}", o.name)));
            }
        }
    }
    // Phase 5: unexpected arguments, listed as CLI11 did (last first).
    if !extras.is_empty() {
        extras.reverse();
        let message = if extras.len() == 1 {
            format!("The following argument was not expected: {}", extras[0])
        } else {
            format!(
                "The following arguments were not expected: {}",
                extras.join(" ")
            )
        };
        return Err(Exit::error(109, message));
    }
    Ok(parsed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn code(line: &str) -> i32 {
        let args: Vec<OsString> = std::iter::once("mradm")
            .chain(line.split(' ').filter(|s| !s.is_empty()))
            .map(OsString::from)
            .collect();
        match parse(args, "0") {
            Ok(_) => 0,
            Err(exit) => exit.code,
        }
    }

    #[test]
    fn exit_codes_follow_cli11() {
        for (line, expected) in [
            ("", 106),
            ("frobnicate", 106),
            ("--nope", 106),
            ("--version", 0),
            ("--version extra", 0),
            ("--help", 0),
            ("frobnicate --help", 0),
            ("render -i", 114),
            ("render -i a -o", 114),
            ("render --help -i", 114),
            ("render -i a --sofa=", 114),
            ("render -i=a -o=x.wav", 0),
            ("render -i a -i b", 114),
            ("render -i a --interp-ms fast --interp-ms 2", 105),
            (
                "render -i a --final-gain-db 1 --final-gain-db 2 --interp-ms bad",
                114,
            ),
            ("render -i a --interp-ms 0x10", 0),
            ("render -i a --interp-ms 501", 105),
            ("render -i a --no-peak-limit=maybe", 104),
            ("render -i a --no-peak-limit=maybe --no-peak-limit=true", 0),
            ("render -i a --apac-drc-music --apac-drc-none=x", 104),
            ("render -i a --apac-drc-none=x --apac-drc-music", 0),
            ("render -i a --renderer EAR", 105),
            ("render -i a --input-layout bad --renderer bad", 105),
            ("render -i a --start -1", 105),
            ("render -o x.wav --room-quality realtime", 106),
            ("render -i a --room-quality realtime", 107),
            ("render -i a --input-layout 5.1 --input-channels L", 108),
            ("render -i a extra", 109),
            ("render -i a --x --y", 109),
            ("render -i a --interp-ms fast --no-such", 105),
            ("render -i a --help --no-such", 0),
            ("render -i a -h --interp-ms fast", 0),
            ("render --version", 106),
            ("render -- -i a", 106),
            ("render -i -o x.wav", 109),
            ("render -o -i a", 106),
            ("render -i a -vv --verbose", 0),
            ("render -i a --verbose=maybe", 104),
            ("-v render -i a", 109),
            ("inspect", 106),
            ("inspect a b", 109),
            ("inspect a --xml=true", 0),
            ("inspect a --xml=maybe", 104),
            ("layouts", 106),
            ("layouts --format WAV", 105),
            ("layouts --format wav --format flac", 114),
            ("formats extra", 109),
            ("room-check", 106),
            ("room-check m --hrir a --sofa b", 114),
            ("room-check m --quality fast", 105),
            ("room-check --quality realtime m", 0),
            ("__apac-encode", 106),
            ("__apac-encode --in a --out b --layout x --bitrate -1", 104),
            ("__apac-encode --in a --out b --layout x --drc maybe", 104),
            ("__apac-encode --in a --out b --layout x --drc 2", 0),
            ("export -i a", 106),
            ("help", 106),
        ] {
            assert_eq!(code(line), expected, "{line:?}");
        }
    }

    #[test]
    fn range_bounds_print_as_fmt_did() {
        assert_eq!(bound(f64::MAX), "1.7976931348623157e+308");
        assert_eq!(bound(f64::MIN), "-1.7976931348623157e+308");
        assert_eq!(bound(0.01), "0.01");
        assert_eq!(bound(100.0), "100");
        assert_eq!(bound32(-60.0), "-60");
    }
}
