// Fixed C ABI call sequences whose public outputs are written for a byte-for-byte comparison of two
// implementations of include/adm/c_api.h (phase 6). Only the C ABI and the standard library are used, and
// the tool links the shared mradm_capi library, so the same binary runs against either implementation.
//
//   mr_adm_capi_capture <out-dir> [family...]
//
// Runs from the record directory (scripts/consistency/capi-capture.py): inputs are read from fixtures/ and
// written below <out-dir>/files/<case>/, every recorded value goes to <out-dir>/capture.txt. Values are
// recorded exactly: doubles and floats as their bit patterns, strings JSON-escaped. Outputs that depend on
// timing (ring fill, underruns, queue depths, ages, bound ports) or on the audio devices of the host are not
// recorded; the device-bound monitor and scene-output paths are covered by the existing C ABI tests.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/c_api.h"

namespace {

namespace fs = std::filesystem;

std::string quote(const char* text) {
    if (text == nullptr) {
        return "null";
    }
    std::string out = "\"";
    for (const char character : std::string_view{text}) {
        const auto c = static_cast<unsigned char>(character);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20U) {
            std::array<char, 8> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "\\u%04x", c);
            out += buffer.data();
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

std::string bits(double value) {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(),
                  buffer.size(),
                  "d:%016llx",
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(value)));
    return buffer.data();
}

// An out-of-range enum value as a C caller passes it: the int bits in an enum-sized argument.
template <class Enum> Enum invalid(int value) {
    static_assert(sizeof(Enum) == sizeof(int));
    Enum out{};
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

class Recorder {
  public:
    explicit Recorder(const fs::path& root) : root_(root), out_(root / "capture.txt", std::ios::binary) {}

    [[nodiscard]] bool ok() const { return static_cast<bool>(out_); }

    void begin(const std::string& name) {
        case_ = name;
        out_ << "[" << name << "]\n";
        fs::create_directories(files());
    }

    [[nodiscard]] fs::path files() const { return root_ / "files" / case_; }

    // A path below files/<case>/, relative to the working directory (the record directory).
    [[nodiscard]] std::string output(const std::string& name) const {
        return (root_ / "files" / case_ / name).generic_string();
    }

    void put(const std::string& key, const std::string& value) { out_ << key << '=' << value << '\n'; }
    void put(const std::string& key, const char* value) { put(key, quote(value)); }
    void put(const std::string& key, long long value) { put(key, std::to_string(value)); }
    void put(const std::string& key, unsigned long long value) { put(key, std::to_string(value)); }
    void put(const std::string& key, int value) { put(key, static_cast<long long>(value)); }
    void put(const std::string& key, unsigned value) { put(key, static_cast<unsigned long long>(value)); }
    void put(const std::string& key, unsigned long value) { put(key, static_cast<unsigned long long>(value)); }
    void put(const std::string& key, long value) { put(key, static_cast<long long>(value)); }
    void code(const std::string& key, adm_error_code_t value) { put(key, static_cast<int>(value)); }

    // Record and free a library-owned string.
    void owned(const std::string& key, char* value) {
        put(key, value);
        adm_free_string(value);
    }

    void write_file(const std::string& name, const void* data, std::size_t bytes) {
        std::ofstream file(files() / name, std::ios::binary);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    }

  private:
    fs::path root_;
    std::ofstream out_;
    std::string case_;
};

struct Context {
    adm_context_t* value{adm_create_context()};
    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
    ~Context() { adm_destroy_context(value); }
};

struct Options {
    adm_render_options_t* value{adm_create_render_options()};
    Options() = default;
    Options(const Options&) = delete;
    Options& operator=(const Options&) = delete;
    Options(Options&&) = delete;
    Options& operator=(Options&&) = delete;
    ~Options() { adm_destroy_render_options(value); }
};

std::string fixture(std::string_view name) {
    return "fixtures/adm/" + std::string{name} + ".wav";
}

std::string read_text(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// ── Progress ─────────────────────────────────────────────────────────────────

struct ProgressLog {
    std::mutex mutex;
    std::vector<std::string> events;
};

void progress_v2(const adm_progress_event_v2_t* event, void* user_data) {
    auto* log = static_cast<ProgressLog*>(user_data);
    std::ostringstream line;
    line << event->struct_size << ' ' << event->stage << ' ' << event->operation << ' ' << bits(event->overall_fraction)
         << ' ' << bits(event->stage_fraction) << ' ' << event->current_frame << ' ' << event->total_frames << ' '
         << quote(event->message);
    const std::scoped_lock lock(log->mutex);
    log->events.push_back(line.str());
}

void progress_v1(double fraction, const char* stage, const char* message, void* user_data) {
    auto* log = static_cast<ProgressLog*>(user_data);
    std::ostringstream line;
    line << bits(fraction) << ' ' << quote(stage) << ' ' << quote(message) << ' '
         << static_cast<int>(adm_render_stage_from_string(stage));
    const std::scoped_lock lock(log->mutex);
    log->events.push_back(line.str());
}

void record_progress(Recorder& rec, const ProgressLog& log) {
    rec.put("progress.count", static_cast<unsigned long long>(log.events.size()));
    for (std::size_t i = 0; i < log.events.size(); ++i) {
        rec.put("progress." + std::to_string(i), log.events[i]);
    }
}

void record_result(Recorder& rec, const adm_render_result_t* result) {
    rec.put("result.present", result != nullptr ? 1 : 0);
    if (result == nullptr) {
        return;
    }
    rec.code("result.code", adm_render_result_error_code(result));
    rec.put("result.message", adm_render_result_message(result));
    rec.put("result.output_path", adm_render_result_output_path(result));
    double value = 0.0;
    const int has_lufs = adm_render_result_loudness_lufs(result, &value);
    rec.put("result.lufs", has_lufs != 0 ? bits(value) : std::string{"absent"});
    value = 0.0;
    const int has_peak = adm_render_result_peak_dbtp(result, &value);
    rec.put("result.peak_dbtp", has_peak != 0 ? bits(value) : std::string{"absent"});
    rec.put("result.semantic_report", adm_render_result_semantic_report_json(result));
    const std::uint32_t logs = adm_render_result_log_count(result);
    rec.put("result.log_count", logs);
    for (std::uint32_t i = 0; i < logs; ++i) {
        adm_log_level_t level = ADM_LOG_DEBUG;
        const char* module = nullptr;
        const char* message = nullptr;
        const int found = adm_render_result_log_entry(result, i, &level, &module, &message);
        rec.put("result.log." + std::to_string(i),
                std::to_string(found) + ' ' + std::to_string(static_cast<int>(level)) + ' ' + quote(module) + ' ' +
                    quote(message));
    }
}

// ── B: version, context and memory ───────────────────────────────────────────

void family_base(Recorder& rec) {
    rec.begin("base.version");
    rec.put("major", adm_api_version_major());
    rec.put("minor", adm_api_version_minor());
    rec.put("patch", adm_api_version_patch());

    rec.begin("base.context");
    {
        const Context context;
        rec.put("created", context.value != nullptr ? 1 : 0);
        rec.put("last_error.initial", adm_context_last_error_message(context.value));
        char* json = nullptr;
        rec.code("inspect.missing", adm_inspect_file_json(context.value, "fixtures/missing/absent.wav", &json));
        rec.put("inspect.missing.out", json);
        rec.put("last_error.after_failure", adm_context_last_error_message(context.value));
        rec.code("capabilities", adm_capabilities_json(context.value, &json));
        adm_free_string(json);
        rec.put("last_error.after_success", adm_context_last_error_message(context.value));
    }
    rec.put("last_error.null_context", adm_context_last_error_message(nullptr));
    adm_free_string(nullptr);
    adm_destroy_context(nullptr);
    rec.put("free_string.null", "ok");

    rec.begin("base.stage_from_string");
    for (const char* stage : {"validating",
                              "probing",
                              "importing scene",
                              "planning",
                              "rendering",
                              "post-processing",
                              "post_processing",
                              "finished",
                              "",
                              "RENDERING",
                              "nonsense"}) {
        rec.put(std::string{"stage."} + stage, static_cast<int>(adm_render_stage_from_string(stage)));
    }
    rec.put("stage.null", static_cast<int>(adm_render_stage_from_string(nullptr)));
}

// ── C: render, inspect, export, room and catalogues ──────────────────────────

void family_catalog(Recorder& rec, const Context& context) {
    rec.begin("render.catalog");
    using Query = adm_error_code_t (*)(adm_context_t*, char**);
    const std::array<std::pair<const char*, Query>, 5> queries{{{"capabilities", adm_capabilities_json},
                                                                {"layouts", adm_layouts_json},
                                                                {"input_layouts", adm_input_layouts_json},
                                                                {"output_formats", adm_output_formats_json},
                                                                {"support_matrix", adm_render_support_matrix_json}}};
    for (const auto& [name, query] : queries) {
        char* json = nullptr;
        rec.code(std::string{name} + ".code", query(context.value, &json));
        rec.owned(name, json);
        rec.code(std::string{name} + ".null_out", query(context.value, nullptr));
    }
}

void family_probe(Recorder& rec, const Context& context) {
    rec.begin("render.probe");
    for (const char* name : {"objects-point", "objects-extent-multi", "directspeakers", "hoa", "policy-objects"}) {
        adm_scene_info_t* info = nullptr;
        const std::string key = std::string{"probe."} + name;
        rec.code(key + ".code", adm_probe_file(context.value, fixture(name).c_str(), &info));
        if (info != nullptr) {
            rec.put(key + ".sample_rate", adm_scene_info_sample_rate(info));
            rec.put(key + ".channels", adm_scene_info_channels(info));
            rec.put(key + ".frames", static_cast<unsigned long long>(adm_scene_info_frames(info)));
            rec.put(key + ".duration", bits(adm_scene_info_duration_seconds(info)));
            rec.put(key + ".programmes", adm_scene_info_programme_count(info));
            rec.put(key + ".objects", adm_scene_info_object_count(info));
        }
        adm_destroy_scene_info(info);
    }
    for (const char* path : {"fixtures/missing/absent.wav", "fixtures/text/not-wave.wav"}) {
        adm_scene_info_t* info = reinterpret_cast<adm_scene_info_t*>(&rec); // NOLINT: must be reset on failure
        const std::string key = std::string{"probe."} + path;
        rec.code(key + ".code", adm_probe_file(context.value, path, &info));
        rec.put(key + ".out_reset", info == nullptr ? 1 : 0);
        rec.put(key + ".last_error", adm_context_last_error_message(context.value));
    }
    rec.put("info.null.sample_rate", adm_scene_info_sample_rate(nullptr));
    rec.put("info.null.frames", static_cast<unsigned long long>(adm_scene_info_frames(nullptr)));
    rec.put("info.null.duration", bits(adm_scene_info_duration_seconds(nullptr)));
    adm_destroy_scene_info(nullptr);
}

void family_inspect(Recorder& rec, const Context& context) {
    rec.begin("render.inspect");
    for (const char* name : {"objects-point", "objects-extent-multi", "directspeakers", "hoa", "policy-objects"}) {
        const std::string key = name;
        char* text = nullptr;
        rec.code(key + ".json.code", adm_inspect_file_json(context.value, fixture(name).c_str(), &text));
        rec.owned(key + ".json", text);
        text = nullptr;
        rec.code(key + ".xml.code", adm_inspect_file_xml(context.value, fixture(name).c_str(), &text));
        rec.owned(key + ".xml", text);
        text = nullptr;
        rec.code(key + ".template.code", adm_policy_template_json(context.value, fixture(name).c_str(), &text));
        rec.owned(key + ".template", text);
    }
    char* text = nullptr;
    rec.code("not_wave.json.code", adm_inspect_file_json(context.value, "fixtures/text/not-wave.wav", &text));
    rec.put("not_wave.json", text);
    rec.put("not_wave.last_error", adm_context_last_error_message(context.value));
    rec.code("null_path.json.code", adm_inspect_file_json(context.value, nullptr, &text));
    rec.put("null_path.last_error", adm_context_last_error_message(context.value));
}

void family_options(Recorder& rec) {
    rec.begin("render.options");
    const Options options;
    auto* o = options.value;
    rec.code("renderer.valid", adm_render_options_set_renderer(o, ADM_RENDERER_EAR));
    rec.code("renderer.invalid", adm_render_options_set_renderer(o, invalid<adm_renderer_t>(99)));
    rec.code("output_layout.valid", adm_render_options_set_output_layout(o, "7.1.4"));
    rec.code("output_layout.null", adm_render_options_set_output_layout(o, nullptr));
    rec.code("input_layout.valid", adm_render_options_set_input_layout(o, "5.1"));
    rec.code("input_layout.unknown", adm_render_options_set_input_layout(o, "9.9.9"));
    rec.code("input_layout.clear", adm_render_options_set_input_layout(o, ""));
    rec.code("input_labels.valid", adm_render_options_set_input_channel_labels(o, "L,R,C,LFE,Ls,Rs"));
    rec.code("input_labels.clear", adm_render_options_set_input_channel_labels(o, nullptr));
    rec.code("bit_depth.invalid", adm_render_options_set_output_bit_depth(o, invalid<adm_output_bit_depth_t>(9)));
    rec.code("loudness.valid", adm_render_options_set_loudness_target(o, -23.0));
    rec.code("loudness.high", adm_render_options_set_loudness_target(o, 5.0));
    rec.code("peak_limit_dbtp.valid", adm_render_options_set_peak_limit_dbtp(o, -1.0));
    rec.code("peak_limit_dbtp.high", adm_render_options_set_peak_limit_dbtp(o, 3.0));
    rec.code("opus_bitrate.valid", adm_render_options_set_opus_bitrate_per_ch_kbps(o, 96));
    rec.code("opus_bitrate.huge", adm_render_options_set_opus_bitrate_per_ch_kbps(o, 100000));
    rec.code("apac_bitrate.valid", adm_render_options_set_apac_bitrate_kbps(o, 1024));
    rec.code("apac_container.invalid", adm_render_options_set_apac_container(o, invalid<adm_apac_container_t>(9)));
    rec.code("interp.valid", adm_render_options_set_default_interp_ms(o, 0));
    rec.code("interp.high", adm_render_options_set_default_interp_ms(o, 501));
    rec.code("smoothing.valid", adm_render_options_set_object_smoothing_frames(o, 48000));
    rec.code("smoothing.high", adm_render_options_set_object_smoothing_frames(o, 48001));
    rec.code("speaker_spread.invalid",
             adm_render_options_set_speaker_spread_mode(o, invalid<adm_speaker_spread_mode_t>(9)));
    rec.code("triple_balance.invalid",
             adm_render_options_set_triple_balance_mode(o, invalid<adm_triple_balance_mode_t>(9)));
    rec.code("binaural_spread.invalid",
             adm_render_options_set_binaural_spread_mode(o, invalid<adm_binaural_spread_mode_t>(9)));
    rec.code("geometry.invalid", adm_render_options_set_speaker_geometry(o, invalid<adm_speaker_geometry_t>(9)));
    rec.code("ds_routing.invalid",
             adm_render_options_set_direct_speakers_routing_mode(o, invalid<adm_direct_speakers_routing_mode_t>(9)));
    rec.code("ds_matrix_json.invalid", adm_render_options_set_direct_speakers_matrix_json(o, "{not json"));
    rec.code("ds_matrix_json.clear", adm_render_options_set_direct_speakers_matrix_json(o, nullptr));
    rec.code("lfe_routing.invalid", adm_render_options_set_lfe_routing_mode(o, invalid<adm_lfe_routing_mode_t>(9)));
    rec.code("iamf_container.invalid", adm_render_options_set_iamf_container(o, invalid<adm_iamf_container_t>(9)));
    rec.code("iamf_layers.valid", adm_render_options_set_iamf_layers(o, "stereo,5.1"));
    rec.code("iamf_layers.invalid", adm_render_options_set_iamf_layers(o, "stereo,bogus"));
    rec.code("start.negative", adm_render_options_set_render_start_sec(o, -1.0));
    rec.code("end.valid", adm_render_options_set_render_end_sec(o, 1.0));
    rec.code("final_gain.nan", adm_render_options_set_final_gain_db(o, std::numeric_limits<double>::quiet_NaN()));
    rec.code("policy_json.invalid", adm_render_options_set_semantic_policy_json(o, "[]"));
    rec.code("policy_path.clear", adm_render_options_set_semantic_policy_path(o, ""));
    rec.code("report_path.clear", adm_render_options_set_semantic_report_path(o, nullptr));
    rec.code("screen_json.invalid", adm_render_options_set_screen_config_json(o, "{\"x\":"));
    rec.code("room_quality.invalid", adm_render_options_set_room_quality(o, invalid<adm_room_quality_t>(9)));
    rec.code("room.clear", adm_render_options_set_room(o, nullptr));
    rec.code("sofa.clear", adm_render_options_set_sofa_path(o, nullptr));
    rec.code("ds_matrix_path.clear", adm_render_options_set_direct_speakers_matrix_path(o, nullptr));
    adm_render_options_set_apac_drc_music(o, 0);
    adm_render_options_set_apple_speaker_rendering_flags(o, 1);
    adm_render_options_set_monitor_system_spatial(o, 1);
    adm_render_options_set_apac_drc_music(nullptr, 0);
    adm_render_options_set_apple_speaker_rendering_flags(nullptr, 1);
    adm_render_options_set_monitor_system_spatial(nullptr, 1);
    rec.code("null.renderer", adm_render_options_set_renderer(nullptr, ADM_RENDERER_EAR));
    rec.code("null.layout", adm_render_options_set_output_layout(nullptr, "5.1"));
    rec.code("null.loudness", adm_render_options_set_loudness_target(nullptr, 5.0));
    adm_render_options_set_peak_limit(nullptr, 1);
    adm_render_options_set_cancel_token(nullptr, nullptr);
    adm_destroy_render_options(nullptr);
    rec.put("null.void_setters", "ok");
}

struct RenderCase {
    const char* name;
    const char* input;
    const char* output;
    void (*configure)(adm_render_options_t*);
};

void render_with(Recorder& rec, const Context& context, const RenderCase& c) {
    rec.begin(std::string{"render.file."} + c.name);
    const Options options;
    if (c.configure != nullptr) {
        c.configure(options.value);
    }
    ProgressLog progress;
    adm_render_result_t* result = nullptr;
    const auto output = rec.output(c.output);
    rec.code(
        "code",
        adm_render_file_ex2(
            context.value, fixture(c.input).c_str(), output.c_str(), options.value, progress_v2, &progress, &result));
    rec.put("last_error", adm_context_last_error_message(context.value));
    record_result(rec, result);
    record_progress(rec, progress);
    adm_destroy_render_result(result);
}

void family_render(Recorder& rec, const Context& context) {
    const std::array<RenderCase, 19> cases{{
        {"binaural-default", "objects-point", "out.wav", nullptr},
        {"binaural-no-peak-limit",
         "objects-point",
         "out.wav",
         [](adm_render_options_t* o) { adm_render_options_set_peak_limit(o, 0); }},
        {"binaural-restored-peak-limit",
         "objects-point",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_peak_limit(o, 0);
             adm_render_options_set_peak_limit(o, 1);
         }},
        {"binaural-flac",
         "objects-extent",
         "out.flac",
         [](adm_render_options_t* o) { adm_render_options_set_renderer(o, ADM_RENDERER_SAF_BINAURAL); }},
        {"binaural-sofa",
         "objects-point",
         "out.wav",
         [](adm_render_options_t* o) { adm_render_options_set_sofa_path(o, "fixtures/sofa/simple.sofa"); }},
        {"vbap-51-flac",
         "objects-point",
         "out.flac",
         [](adm_render_options_t* o) { adm_render_options_set_output_layout(o, "5.1"); }},
        {"vbap-714-mka",
         "objects-extent",
         "out.mka",
         [](adm_render_options_t* o) { adm_render_options_set_output_layout(o, "7.1.4"); }},
        {"vbap-714-flac-unsupported",
         "objects-point",
         "out.flac",
         [](adm_render_options_t* o) { adm_render_options_set_output_layout(o, "7.1.4"); }},
        {"ear-51-i24",
         "objects-extent",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_EAR);
             adm_render_options_set_output_layout(o, "5.1");
             adm_render_options_set_output_bit_depth(o, ADM_BIT_DEPTH_I24);
         }},
        {"hoa3-caf",
         "objects-point",
         "out.caf",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_HOA);
             adm_render_options_set_output_layout(o, "hoa3");
         }},
        {"triple-balance",
         "objects-cartesian",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_TRIPLE_BALANCE);
             adm_render_options_set_output_layout(o, "7.1.4");
             adm_render_options_set_speaker_spread_mode(o, ADM_SPEAKER_SPREAD_NONE);
         }},
        {"ds-matrix-json",
         "directspeakers",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_EAR);
             adm_render_options_set_output_layout(o, "5.1");
             adm_render_options_set_direct_speakers_routing_mode(o, ADM_DIRECT_SPEAKERS_ROUTING_MATRIX);
             adm_render_options_set_direct_speakers_matrix_json(o,
                                                                read_text("fixtures/json/ds-matrix-51.json").c_str());
         }},
        {"policy-report",
         "policy-objects",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_EAR);
             adm_render_options_set_output_layout(o, "7.1.4");
             adm_render_options_set_semantic_policy_json(o, read_text("fixtures/json/policy-identity.json").c_str());
             adm_render_options_set_capture_semantic_report(o, 1);
         }},
        {"loudness-trim-gain",
         "objects-extent",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_loudness_target(o, -23.0);
             adm_render_options_set_render_start_sec(o, 0.25);
             adm_render_options_set_render_end_sec(o, 0.75);
             adm_render_options_set_final_gain_db(o, -1.5);
         }},
        {"peak-normalize-i16",
         "objects-point",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_output_layout(o, "5.1");
             adm_render_options_set_peak_normalize_to_limit(o, 1);
             adm_render_options_set_peak_limit_dbtp(o, -2.0);
             adm_render_options_set_output_bit_depth(o, ADM_BIT_DEPTH_I16);
         }},
        {"smoothing-hoa",
         "objects-room",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_renderer(o, ADM_RENDERER_HOA);
             adm_render_options_set_output_layout(o, "hoa3");
             adm_render_options_set_object_smoothing_frames(o, 2048);
             adm_render_options_set_default_interp_ms(o, 0);
         }},
        {"input-layout",
         "directspeakers",
         "out.wav",
         [](adm_render_options_t* o) { adm_render_options_set_input_layout(o, "5.1"); }},
        {"ds-matrix-path",
         "directspeakers",
         "out.wav",
         [](adm_render_options_t* o) {
             adm_render_options_set_output_layout(o, "5.1");
             adm_render_options_set_direct_speakers_routing_mode(o, ADM_DIRECT_SPEAKERS_ROUTING_MATRIX);
             adm_render_options_set_direct_speakers_matrix_path(o, "fixtures/json/ds-matrix-51.json");
             adm_render_options_set_monitor_system_spatial(o, 1); // monitor-only; offline ignores it
         }},
        {"apple-unavailable",
         "objects-point",
         "out.wav",
         [](adm_render_options_t* o) { adm_render_options_set_renderer(o, ADM_RENDERER_APPLE); }},
    }};
    for (const auto& c : cases) {
        render_with(rec, context, c);
    }

    rec.begin("render.file.legacy");
    {
        ProgressLog progress;
        adm_render_result_t* result = nullptr;
        const auto output = rec.output("out.wav");
        rec.code("render_file",
                 adm_render_file(
                     context.value, fixture("objects-point").c_str(), output.c_str(), progress_v1, &progress, &result));
        record_result(rec, result);
        record_progress(rec, progress);
        adm_destroy_render_result(result);
        const Options options;
        adm_render_options_set_output_layout(options.value, "5.1");
        result = nullptr;
        ProgressLog progress_ex;
        const auto output_ex = rec.output("out-ex.wav");
        rec.code("render_file_ex",
                 adm_render_file_ex(context.value,
                                    fixture("objects-point").c_str(),
                                    output_ex.c_str(),
                                    options.value,
                                    progress_v1,
                                    &progress_ex,
                                    &result));
        record_result(rec, result);
        rec.put("progress_ex.count", static_cast<unsigned long long>(progress_ex.events.size()));
        adm_destroy_render_result(result);
    }

    rec.begin("render.file.errors");
    for (const auto& [name, input] :
         std::array<std::pair<const char*, const char*>, 3>{{{"missing", "fixtures/missing/absent.wav"},
                                                             {"not_wave", "fixtures/text/not-wave.wav"},
                                                             {"null", nullptr}}}) {
        adm_render_result_t* result = nullptr;
        const auto output = rec.output(std::string{name} + ".wav");
        rec.code(std::string{name} + ".code",
                 adm_render_file_ex2(context.value, input, output.c_str(), nullptr, nullptr, nullptr, &result));
        rec.put(std::string{name} + ".last_error", adm_context_last_error_message(context.value));
        record_result(rec, result);
        adm_destroy_render_result(result);
    }
    {
        const Options options;
        adm_render_options_set_render_start_sec(options.value, 0.75);
        adm_render_options_set_render_end_sec(options.value, 0.25);
        adm_render_result_t* result = nullptr;
        const auto output = rec.output("trim.wav");
        rec.code("trim_inverted.code",
                 adm_render_file_ex2(context.value,
                                     fixture("objects-point").c_str(),
                                     output.c_str(),
                                     options.value,
                                     nullptr,
                                     nullptr,
                                     &result));
        record_result(rec, result);
        adm_destroy_render_result(result);
    }

    rec.begin("render.cancel");
    {
        adm_cancel_token_t* token = adm_create_cancel_token();
        const Options options;
        adm_render_options_set_cancel_token(options.value, token);
        adm_cancel(token);
        adm_render_result_t* result = nullptr;
        const auto output = rec.output("cancelled.wav");
        rec.code("cancelled.code",
                 adm_render_file_ex2(context.value,
                                     fixture("objects-point").c_str(),
                                     output.c_str(),
                                     options.value,
                                     nullptr,
                                     nullptr,
                                     &result));
        record_result(rec, result);
        adm_destroy_render_result(result);
        adm_reset_cancel_token(token);
        result = nullptr;
        const auto again = rec.output("after-reset.wav");
        rec.code("after_reset.code",
                 adm_render_file_ex2(context.value,
                                     fixture("objects-point").c_str(),
                                     again.c_str(),
                                     options.value,
                                     nullptr,
                                     nullptr,
                                     &result));
        record_result(rec, result);
        adm_destroy_render_result(result);
        adm_render_options_set_cancel_token(options.value, nullptr);
        adm_destroy_cancel_token(token);
        adm_cancel(nullptr);
        adm_reset_cancel_token(nullptr);
        adm_destroy_cancel_token(nullptr);
    }

    rec.begin("render.result.null");
    rec.code("code", adm_render_result_error_code(nullptr));
    rec.put("message", adm_render_result_message(nullptr));
    rec.put("output_path", adm_render_result_output_path(nullptr));
    double value = 1.0;
    rec.put("lufs", adm_render_result_loudness_lufs(nullptr, &value));
    rec.put("lufs.value", bits(value));
    rec.put("log_count", adm_render_result_log_count(nullptr));
    adm_destroy_render_result(nullptr);
}

void family_export(Recorder& rec, const Context& context) {
    rec.begin("render.export");
    {
        const Options options;
        adm_render_options_set_semantic_policy_json(options.value,
                                                    read_text("fixtures/json/policy-position.json").c_str());
        rec.code(
            "policy.code",
            adm_export_file(
                context.value, fixture("policy-objects").c_str(), rec.output("policy.wav").c_str(), options.value));
    }
    rec.code(
        "plain.code",
        adm_export_file(context.value, fixture("objects-point").c_str(), rec.output("plain.wav").c_str(), nullptr));
    rec.code("missing.code",
             adm_export_file(context.value, "fixtures/missing/absent.wav", rec.output("missing.wav").c_str(), nullptr));
    rec.put("missing.last_error", adm_context_last_error_message(context.value));
}

void family_room(Recorder& rec, const Context& context) {
    rec.begin("render.room");
    adm_room_t* room = nullptr;
    rec.code("load.code", adm_room_load(context.value, "fixtures/room/room.json", &room));
    rec.put("manifest", room != nullptr ? adm_room_manifest_json(room) : nullptr);
    adm_room_t* missing = nullptr;
    rec.code("load.missing", adm_room_load(context.value, "fixtures/missing/room.json", &missing));
    rec.put("load.missing.last_error", adm_context_last_error_message(context.value));
    rec.code("load.not_room", adm_room_load(context.value, "fixtures/json/policy-identity.json", &missing));
    rec.put("load.not_room.last_error", adm_context_last_error_message(context.value));
    if (room != nullptr) {
        adm_room_config_t config{};
        config.struct_size = sizeof(config);
        config.room = room;
        char* json = nullptr;
        rec.code("inspect.code", adm_room_inspect(context.value, &config, &json));
        rec.owned("inspect", json);
        for (const auto& [name, quality] : std::array<std::pair<const char*, adm_room_quality_t>, 2>{
                 {{"exact", ADM_ROOM_QUALITY_EXACT}, {"realtime", ADM_ROOM_QUALITY_REALTIME}}}) {
            json = nullptr;
            rec.code(std::string{"inspect."} + name + ".code",
                     adm_room_inspect_with_quality(context.value, &config, quality, &json));
            rec.owned(std::string{"inspect."} + name, json);
        }
        config.hrir_manifest = "fixtures/room/hrir.json";
        config.metres_per_unit = 2.0;
        json = nullptr;
        rec.code("inspect.hrir.code", adm_room_inspect(context.value, &config, &json));
        rec.owned("inspect.hrir", json);
        config.metres_per_unit = 1000.0;
        json = nullptr;
        rec.code("inspect.bad_scale.code", adm_room_inspect(context.value, &config, &json));
        rec.put("inspect.bad_scale", json);
        rec.put("inspect.bad_scale.last_error", adm_context_last_error_message(context.value));

        config.hrir_manifest = nullptr;
        config.metres_per_unit = 0.0;
        const Options options;
        adm_render_options_set_renderer(options.value, ADM_RENDERER_ROOM_BINAURAL);
        rec.code("options.set_room", adm_render_options_set_room(options.value, &config));
        adm_room_destroy(room); // the options retain the room
        room = nullptr;
        for (const auto& [name, quality] : std::array<std::pair<const char*, adm_room_quality_t>, 2>{
                 {{"exact", ADM_ROOM_QUALITY_EXACT}, {"realtime", ADM_ROOM_QUALITY_REALTIME}}}) {
            adm_render_options_set_room_quality(options.value, quality);
            adm_render_options_set_screen_config_json(options.value,
                                                      read_text("fixtures/json/screen-config.json").c_str());
            adm_render_result_t* result = nullptr;
            const auto output = rec.output(std::string{"room-"} + name + ".wav");
            rec.code(std::string{"render."} + name + ".code",
                     adm_render_file_ex2(context.value,
                                         fixture("objects-extent-multi").c_str(),
                                         output.c_str(),
                                         options.value,
                                         nullptr,
                                         nullptr,
                                         &result));
            record_result(rec, result);
            adm_destroy_render_result(result);
        }
    }
    adm_room_destroy(room);
    adm_room_destroy(nullptr);
    rec.put("manifest.null", adm_room_manifest_json(nullptr));
}

// ── D: preview ───────────────────────────────────────────────────────────────

void family_preview(Recorder& rec, const Context& context) {
    rec.begin("preview.session");
    const Options options;
    adm_render_options_set_output_layout(options.value, "5.1");
    adm_preview_session_t* session = nullptr;
    rec.code("create.code",
             adm_create_preview_session(context.value, fixture("objects-extent").c_str(), options.value, &session));
    if (session != nullptr) {
        const std::array<std::pair<double, double>, 4> windows{{{0.0, 0.5}, {0.25, 0.75}, {0.5, 1.0}, {0.9, 2.0}}};
        for (std::size_t i = 0; i < windows.size(); ++i) {
            ProgressLog progress;
            adm_render_result_t* result = nullptr;
            const auto output = rec.output("window-" + std::to_string(i) + ".wav");
            rec.code(
                "window." + std::to_string(i) + ".code",
                adm_preview_render_window_v2(
                    session, windows[i].first, windows[i].second, output.c_str(), progress_v2, &progress, &result));
            record_result(rec, result);
            record_progress(rec, progress);
            adm_destroy_render_result(result);
        }
        ProgressLog progress;
        adm_render_result_t* result = nullptr;
        const auto output = rec.output("legacy.wav");
        rec.code("legacy.code",
                 adm_preview_render_window(session, 0.1, 0.4, output.c_str(), progress_v1, &progress, &result));
        record_result(rec, result);
        rec.put("legacy.progress_count", static_cast<unsigned long long>(progress.events.size()));
        adm_destroy_render_result(result);
        result = nullptr;
        rec.code("inverted.code",
                 adm_preview_render_window_v2(
                     session, 0.75, 0.25, rec.output("inverted.wav").c_str(), nullptr, nullptr, &result));
        record_result(rec, result);
        adm_destroy_render_result(result);
    }
    adm_destroy_preview_session(session);
    adm_preview_session_t* missing = nullptr;
    rec.code("create.missing",
             adm_create_preview_session(context.value, "fixtures/missing/absent.wav", options.value, &missing));
    rec.put("create.missing.last_error", adm_context_last_error_message(context.value));
    adm_destroy_preview_session(missing);
    adm_destroy_preview_session(nullptr);
}

// ── E: Scene streams ─────────────────────────────────────────────────────────

constexpr std::uint64_t k_element = 7U;

adm_scene_object_state_t object_state(float x, float y, float z, float gain) {
    adm_scene_object_state_t state{};
    state.struct_size = sizeof(state);
    state.valid_fields = ADM_SCENE_STATE_ACTIVE | ADM_SCENE_STATE_LINEAR_GAIN | ADM_SCENE_STATE_POSITION |
                         ADM_SCENE_STATE_EXTENT | ADM_SCENE_STATE_DIFFUSE | ADM_SCENE_STATE_DIVERGENCE |
                         ADM_SCENE_STATE_CHANNEL_LOCK | ADM_SCENE_STATE_SCREEN_REFERENCE | ADM_SCENE_STATE_HEAD_LOCKED |
                         ADM_SCENE_STATE_DIVERGENCE_RANGE | ADM_SCENE_STATE_CHANNEL_LOCK_MAX_DISTANCE;
    state.active = 1;
    state.linear_gain = gain;
    state.position_x = x;
    state.position_y = y;
    state.position_z = z;
    state.divergence_azimuth_range = 45.0F;
    return state;
}

adm_scene_renderer_config_t renderer_config(int32_t renderer, const char* layout) {
    adm_scene_renderer_config_t config{};
    config.struct_size = sizeof(config);
    config.renderer = renderer;
    config.output_layout = layout;
    config.speaker_geometry = ADM_SPEAKER_GEOMETRY_STANDARD;
    config.speaker_spread_mode = ADM_SPEAKER_SPREAD_AUTOMATIC;
    config.binaural_spread_mode = ADM_BINAURAL_SPREAD_AUTOMATIC;
    config.lfe_routing_mode = ADM_LFE_ROUTING_DIRECT;
    return config;
}

adm_scene_stream_config_t stream_config(const adm_scene_renderer_config_t& rendering, std::uint32_t input_rate) {
    adm_scene_stream_config_t config{};
    config.struct_size = sizeof(config);
    config.rendering = rendering;
    config.input_sample_rate = input_rate;
    config.output_sample_rate = 48000U;
    config.input_queue_samples = 1U << 16U;
    config.input_queue_bytes = 1ULL << 22U;
    config.output_ring_frames = 1U << 15U;
    config.startup_watermark_frames = 1U;
    return config;
}

void record_stream_status(Recorder& rec, const std::string& key, adm_scene_stream_t* stream) {
    adm_scene_stream_status_t status{};
    status.struct_size = sizeof(status);
    rec.code(key + ".code", adm_scene_stream_get_status(stream, &status));
    // Queue depths, ring fill and underruns depend on the worker's timing and are not recorded.
    rec.put(key,
            std::to_string(status.state) + " epoch=" + std::to_string(status.epoch_id) + " generation=" +
                std::to_string(status.generation_id) + " pulled=" + std::to_string(status.media_frames_pulled) +
                " degradations=" + std::to_string(status.semantic_degradations) +
                " revision=" + std::to_string(status.semantic_policy_revision) +
                " ended=" + std::to_string(status.ended) + " failed=" + std::to_string(status.failed));
}

void record_stream_logs(Recorder& rec, adm_scene_stream_t* stream) {
    const std::uint32_t count = adm_scene_stream_log_count(stream);
    rec.put("logs.count", count);
    for (std::uint32_t i = 0; i < count; ++i) {
        adm_scene_diagnostic_t entry{};
        entry.struct_size = sizeof(entry);
        const int found = adm_scene_stream_log_entry(stream, i, &entry);
        rec.put("logs." + std::to_string(i),
                std::to_string(found) + ' ' + std::to_string(entry.level) + ' ' + std::to_string(entry.code) + ' ' +
                    std::to_string(entry.epoch_id) + ' ' + std::to_string(entry.generation_id) + ' ' +
                    std::to_string(entry.element_id) + ' ' + std::to_string(entry.field_mask) + ' ' +
                    quote(entry.message));
    }
}

// Submits `frames` frames of `block` samples of a fixed integer-derived signal with one moving object,
// signals the end and pulls every output frame until EOS. The PCM goes to files/<case>/pull.f32.
void run_stream(Recorder& rec, adm_scene_stream_t* stream, std::uint32_t block, std::uint32_t frames) {
    adm_scene_output_format_t format{};
    format.struct_size = sizeof(format);
    rec.code("format.code", adm_scene_stream_get_output_format(stream, &format));
    rec.put("format",
            std::to_string(format.sample_format) + ' ' + std::to_string(format.sample_rate) + ' ' +
                std::to_string(format.channels) + ' ' + std::to_string(format.interleaved));
    rec.code("begin_epoch", adm_scene_stream_begin_epoch(stream, 1U, 0));
    adm_scene_element_descriptor_t descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.role = ADM_SCENE_ELEMENT_OBJECT;
    descriptor.element_id = k_element;
    descriptor.has_position = 1;
    descriptor.position_y = 1.0F;
    rec.code("configure", adm_scene_stream_configure_generation(stream, 1U, 1U, &descriptor, 1U));
    std::vector<float> samples(block);
    for (std::uint32_t f = 0; f < frames; ++f) {
        for (std::uint32_t i = 0; i < block; ++i) {
            const std::uint32_t n = f * block + i;
            samples[i] = static_cast<float>(static_cast<int>((n * 37U) % 200U) - 100) / 256.0F;
        }
        adm_scene_pcm_plane_t plane{};
        plane.struct_size = sizeof(plane);
        plane.element_id = k_element;
        plane.samples = samples.data();
        plane.sample_count = block;
        plane.stride = 1U;
        plane.has_signal = 1;
        adm_scene_initial_state_t initial{};
        initial.struct_size = sizeof(initial);
        initial.element_id = k_element;
        initial.state = object_state(0.0F, 1.0F, 0.0F, 1.0F);
        adm_scene_metadata_update_t update{};
        update.struct_size = sizeof(update);
        update.element_id = k_element;
        update.offset_samples = block / 2U;
        update.ramp_duration_samples = block / 4U;
        update.changed_fields = ADM_SCENE_STATE_POSITION | ADM_SCENE_STATE_LINEAR_GAIN;
        update.state = object_state(static_cast<float>(f + 1U) / static_cast<float>(frames + 1U) - 0.5F,
                                    0.5F,
                                    0.25F,
                                    0.5F + 0.1F * static_cast<float>(f));
        adm_scene_frame_t frame{};
        frame.struct_size = sizeof(frame);
        frame.flags = ADM_SCENE_FRAME_STATE_COMPLETE;
        frame.epoch_id = 1U;
        frame.generation_id = 1U;
        frame.media_sample_start = static_cast<std::int64_t>(f) * block;
        frame.duration_samples = block;
        frame.pcm_count = 1U;
        frame.pcm = &plane;
        frame.initial_state_count = f == 0U ? 1U : 0U;
        frame.initial_states = f == 0U ? &initial : nullptr;
        frame.metadata_update_count = f % 2U == 1U ? 1U : 0U;
        frame.metadata_updates = f % 2U == 1U ? &update : nullptr;
        int32_t submitted = -1;
        rec.code("submit." + std::to_string(f), adm_scene_stream_submit_frame(stream, &frame, 10000U, &submitted));
        rec.put("submit." + std::to_string(f) + ".status", submitted);
    }
    rec.code("signal_end", adm_scene_stream_signal_end(stream, 1U, static_cast<std::int64_t>(frames) * block));
    std::vector<float> pcm;
    std::vector<float> buffer(static_cast<std::size_t>(256U) * std::max<std::uint32_t>(format.channels, 1U));
    bool eos = false;
    bool failed = false;
    std::uint64_t next_frame = 0U;
    bool contiguous = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (!eos && !failed && std::chrono::steady_clock::now() < deadline) {
        adm_scene_pull_result_t pulled{};
        pulled.struct_size = sizeof(pulled);
        if (adm_scene_stream_pull(stream, buffer.data(), 256U, &pulled) != ADM_ERROR_OK) {
            failed = true;
            break;
        }
        if (pulled.media_frames != 0U) {
            contiguous = contiguous && pulled.first_media_frame == next_frame && pulled.epoch_id == 1U;
            next_frame = pulled.first_media_frame + pulled.media_frames;
            pcm.insert(pcm.end(),
                       buffer.begin(),
                       buffer.begin() + static_cast<std::ptrdiff_t>(pulled.media_frames) *
                                            static_cast<std::ptrdiff_t>(format.channels));
        }
        eos = (pulled.flags & ADM_SCENE_PULL_EOS) != 0U;
        failed = (pulled.flags & ADM_SCENE_PULL_FAILED) != 0U;
        if (!eos && pulled.media_frames == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    rec.put("pull.eos", eos ? 1 : 0);
    rec.put("pull.failed", failed ? 1 : 0);
    rec.put("pull.contiguous", contiguous ? 1 : 0);
    rec.put("pull.frames", static_cast<unsigned long long>(next_frame));
    rec.write_file("pull.f32", pcm.data(), pcm.size() * sizeof(float));
    record_stream_status(rec, "status.end", stream);
    record_stream_logs(rec, stream);
}

void stream_case(Recorder& rec,
                 const Context& context,
                 const char* name,
                 const adm_scene_stream_config_t& config,
                 std::uint32_t block,
                 std::uint32_t frames,
                 const char* policy = nullptr) {
    rec.begin(std::string{"scene.stream."} + name);
    adm_scene_stream_t* stream = nullptr;
    rec.code("create", adm_create_scene_stream(context.value, &config, &stream));
    rec.put("create.last_error", adm_context_last_error_message(context.value));
    if (stream == nullptr) {
        return;
    }
    if (policy != nullptr) {
        rec.code("policy", adm_scene_stream_set_semantic_policy_json(stream, policy, 1U));
        rec.put("policy.last_error", adm_scene_stream_last_error_message(stream));
    }
    rec.code("orientation", adm_scene_stream_set_listener_orientation(stream, 15.0F, -5.0F, 0.0F));
    run_stream(rec, stream, block, frames);
    adm_destroy_scene_stream(stream);
}

void family_scene(Recorder& rec, const Context& context) {
    stream_case(rec, context, "vbap-51", stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 48000U), 480U, 20U);
    stream_case(rec,
                context,
                "vbap-714-resampled",
                stream_config(renderer_config(ADM_RENDERER_SAF, "4+7+0"), 44100U),
                441U,
                20U);
    stream_case(rec,
                context,
                "binaural",
                stream_config(renderer_config(ADM_RENDERER_SAF_BINAURAL, "binaural"), 48000U),
                512U,
                16U);
    stream_case(rec,
                context,
                "binaural-resampled",
                stream_config(renderer_config(ADM_RENDERER_SAF_BINAURAL, nullptr), 44100U),
                441U,
                20U);
    stream_case(rec,
                context,
                "triple-balance",
                stream_config(renderer_config(ADM_RENDERER_TRIPLE_BALANCE, "4+7+0"), 48000U),
                480U,
                20U);
    stream_case(rec,
                context,
                "policy",
                stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 48000U),
                480U,
                10U,
                read_text("fixtures/json/policy-lock.json").c_str());

    rec.begin("scene.stream.switch-before-epoch");
    {
        adm_scene_stream_t* stream = nullptr;
        const auto config = stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 48000U);
        rec.code("create", adm_create_scene_stream(context.value, &config, &stream));
        if (stream != nullptr) {
            const auto binaural = renderer_config(ADM_RENDERER_SAF_BINAURAL, "binaural");
            rec.code("switch", adm_scene_stream_switch_backend(stream, &binaural));
            char* error = nullptr;
            const auto stereo = renderer_config(ADM_RENDERER_SAF, "0+2+0");
            rec.code("switch_ex.stereo", adm_scene_stream_switch_backend_ex(stream, &stereo, &error));
            rec.owned("switch_ex.stereo.error", error);
            run_stream(rec, stream, 480U, 10U);
        }
        adm_destroy_scene_stream(stream);
    }

    rec.begin("scene.stream.errors");
    {
        adm_scene_stream_t* stream = nullptr;
        auto stereo = stream_config(renderer_config(ADM_RENDERER_SAF, "0+2+0"), 48000U);
        rec.code("speaker_stereo", adm_create_scene_stream(context.value, &stereo, &stream));
        rec.put("speaker_stereo.last_error", adm_context_last_error_message(context.value));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        auto unknown = stream_config(renderer_config(ADM_RENDERER_SAF, "9+9+9"), 48000U);
        rec.code("unknown_layout", adm_create_scene_stream(context.value, &unknown, &stream));
        rec.put("unknown_layout.last_error", adm_context_last_error_message(context.value));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        auto zero_rate = stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 0U);
        rec.code("zero_rate", adm_create_scene_stream(context.value, &zero_rate, &stream));
        rec.put("zero_rate.last_error", adm_context_last_error_message(context.value));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        rec.code("null_config", adm_create_scene_stream(context.value, nullptr, &stream));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        auto valid = stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 48000U);
        rec.code("valid", adm_create_scene_stream(context.value, &valid, &stream));
        if (stream != nullptr) {
            rec.code("configure_before_epoch", adm_scene_stream_configure_generation(stream, 1U, 1U, nullptr, 0U));
            rec.put("configure_before_epoch.last_error", adm_scene_stream_last_error_message(stream));
            rec.code("policy_invalid", adm_scene_stream_set_semantic_policy_json(stream, "{\"schema\":\"nope\"}", 1U));
            rec.put("policy_invalid.last_error", adm_scene_stream_last_error_message(stream));
            rec.code("begin_epoch_zero", adm_scene_stream_begin_epoch(stream, 0U, 0));
            rec.put("begin_epoch_zero.last_error", adm_scene_stream_last_error_message(stream));
            record_stream_status(rec, "status.idle", stream);
        }
        adm_destroy_scene_stream(stream);
        adm_destroy_scene_stream(nullptr);
        rec.put("null.last_error", adm_scene_stream_last_error_message(nullptr));
        adm_scene_stream_status_t status{};
        status.struct_size = sizeof(status);
        rec.code("null.status", adm_scene_stream_get_status(nullptr, &status));
        rec.put("null.log_count", adm_scene_stream_log_count(nullptr));
    }

    rec.begin("scene.stream.room");
    {
        adm_room_t* room = nullptr;
        rec.code("load", adm_room_load(context.value, "fixtures/room/room.json", &room));
        adm_room_config_t room_config{};
        room_config.struct_size = sizeof(room_config);
        room_config.room = room;
        const auto rendering = renderer_config(ADM_RENDERER_ROOM_BINAURAL, nullptr);
        auto config = stream_config(rendering, 48000U);
        const std::string screen = read_text("fixtures/json/screen-config.json");
        for (const auto& [name, quality] : std::array<std::pair<const char*, adm_room_quality_t>, 2>{
                 {{"exact", ADM_ROOM_QUALITY_EXACT}, {"realtime", ADM_ROOM_QUALITY_REALTIME}}}) {
            adm_scene_stream_t* stream = nullptr;
            rec.code(std::string{"create."} + name,
                     adm_create_scene_stream_with_room_quality(
                         context.value, &config, &room_config, quality, screen.c_str(), &stream));
            rec.put(std::string{"create."} + name + ".last_error", adm_context_last_error_message(context.value));
            if (stream != nullptr) {
                char* json = nullptr;
                rec.code(std::string{"source."} + name + ".unknown",
                         adm_scene_stream_room_source_json(stream, 999U, &json));
                rec.put(std::string{"source."} + name + ".unknown.json", json);
                adm_free_string(json);
            }
            adm_destroy_scene_stream(stream);
        }
        adm_scene_stream_t* stream = nullptr;
        rec.code("create.plain", adm_create_scene_stream_with_room(context.value, &config, &room_config, &stream));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        rec.code("create.ex",
                 adm_create_scene_stream_with_room_ex(context.value, &config, &room_config, screen.c_str(), &stream));
        adm_destroy_scene_stream(stream);
        stream = nullptr;
        auto vbap = stream_config(renderer_config(ADM_RENDERER_SAF, "0+5+0"), 48000U);
        rec.code("create.wrong_renderer",
                 adm_create_scene_stream_with_room(context.value, &vbap, &room_config, &stream));
        rec.put("create.wrong_renderer.last_error", adm_context_last_error_message(context.value));
        adm_destroy_scene_stream(stream);
        adm_room_destroy(room);
    }
}

// ── F: HpTF parsing and the OSC receiver (no audio device) ───────────────────

void family_monitor(Recorder& rec, const Context& context) {
    rec.begin("monitor.hptf_parse");
    const std::array<std::pair<const char*, const char*>, 5> texts{{
        {"autoeq",
         "Preamp: -6.2 dB\nFilter 1: ON PK Fc 105 Hz Gain -3.1 dB Q 0.70\n"
         "Filter 2: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71\nFilter 3: OFF PK Fc 2000 Hz Gain 2.0 dB Q 1.41\n"
         "Filter 4: ON HSC Fc 10000 Hz Gain -2.5 dB Q 0.71\n"},
        {"crlf", "Preamp: -1 dB\r\nFilter 1: ON PK Fc 1000 Hz Gain 1 dB Q 1\r\n"},
        {"empty", ""},
        {"garbage", "Filter 1: ON PK Fc abc Hz Gain 1 dB Q 1\n"},
        {"negative_q", "Preamp: 0 dB\nFilter 1: ON PK Fc 1000 Hz Gain 1 dB Q -1\n"},
    }};
    for (const auto& [name, text] : texts) {
        const std::string key = name;
        double preamp = 0.0;
        std::uint32_t count = 0U;
        rec.code(key + ".probe", adm_hptf_parse_parametric_eq(context.value, text, &preamp, nullptr, 0U, &count));
        rec.put(key + ".probe.preamp", bits(preamp));
        rec.put(key + ".probe.count", count);
        rec.put(key + ".probe.last_error", adm_context_last_error_message(context.value));
        std::vector<adm_hptf_band_t> bands(std::max<std::uint32_t>(count, 1U));
        for (auto& band : bands) {
            band.struct_size = sizeof(band);
        }
        if (count > 1U) {
            std::uint32_t short_count = 0U;
            rec.code(
                key + ".short",
                adm_hptf_parse_parametric_eq(context.value, text, &preamp, bands.data(), count - 1U, &short_count));
            rec.put(key + ".short.count", short_count);
        }
        count = static_cast<std::uint32_t>(bands.size());
        rec.code(key + ".parse",
                 adm_hptf_parse_parametric_eq(context.value, text, &preamp, bands.data(), count, &count));
        rec.put(key + ".parse.count", count);
        for (std::uint32_t i = 0; i < count && i < bands.size(); ++i) {
            const auto& b = bands[i];
            rec.put(key + ".band." + std::to_string(i),
                    std::to_string(b.type) + ' ' + std::to_string(b.enabled) + ' ' + bits(b.fc_hz) + ' ' +
                        bits(b.gain_db) + ' ' + bits(b.q));
        }
    }
    double preamp = 0.0;
    std::uint32_t count = 0U;
    rec.code("null_text", adm_hptf_parse_parametric_eq(context.value, nullptr, &preamp, nullptr, 0U, &count));
    rec.code("null_count",
             adm_hptf_parse_parametric_eq(context.value, "Preamp: 0 dB\n", &preamp, nullptr, 0U, nullptr));

    rec.begin("monitor.osc");
    {
        adm_osc_head_tracking_config_t config{};
        config.struct_size = sizeof(config);
        config.listen_port = 0U;
        config.source_id = "capture";
        adm_osc_head_tracking_t* receiver = nullptr;
        rec.code("create", adm_create_osc_head_tracking(&config, &receiver));
        if (receiver != nullptr) {
            adm_osc_head_tracking_status_t status{};
            status.struct_size = sizeof(status);
            rec.code("status.before", adm_osc_head_tracking_get_status(receiver, &status));
            rec.put("status.before.state", status.state);
            adm_head_tracking_pose_t pose{};
            pose.struct_size = sizeof(pose);
            rec.code("pose.before", adm_osc_head_tracking_get_pose(receiver, &pose));
            rec.put("pose.before.has_pose", pose.has_pose);
            rec.code("start", adm_osc_head_tracking_start(receiver));
            status = {};
            status.struct_size = sizeof(status);
            rec.code("status.started", adm_osc_head_tracking_get_status(receiver, &status));
            rec.put("status.started.state", status.state);
            rec.put("status.started.bound", status.bound_port != 0U ? 1 : 0);
            adm_osc_head_tracking_stop(receiver);
            status = {};
            status.struct_size = sizeof(status);
            rec.code("status.stopped", adm_osc_head_tracking_get_status(receiver, &status));
            rec.put("status.stopped.state", status.state);
            rec.put("last_error", adm_osc_head_tracking_last_error_message(receiver));
            char* json = nullptr;
            // The snapshot carries the bound port and receiver times: only its presence is recorded.
            rec.code("snapshot", adm_osc_head_tracking_snapshot_json(receiver, &json));
            rec.put("snapshot.present", json != nullptr ? 1 : 0);
            adm_free_string(json);
        }
        adm_destroy_osc_head_tracking(receiver);
        config.source_id = "";
        receiver = nullptr;
        rec.code("create.empty_source", adm_create_osc_head_tracking(&config, &receiver));
        adm_destroy_osc_head_tracking(receiver);
        const std::string long_source(257, 'x');
        config.source_id = long_source.c_str();
        receiver = nullptr;
        rec.code("create.long_source", adm_create_osc_head_tracking(&config, &receiver));
        adm_destroy_osc_head_tracking(receiver);
        adm_destroy_osc_head_tracking(nullptr);
        rec.put("null.last_error", adm_osc_head_tracking_last_error_message(nullptr));
        char* json = nullptr;
        rec.code("null.snapshot", adm_osc_head_tracking_snapshot_json(nullptr, &json));
    }

    rec.begin("monitor.null");
    rec.put("last_error", adm_monitor_last_error_message(nullptr));
    rec.code("play", adm_monitor_play(nullptr));
    rec.code("pause", adm_monitor_pause(nullptr));
    rec.code("seek", adm_monitor_seek(nullptr, 1.0));
    rec.code("loop", adm_monitor_set_loop(nullptr, 0.0, 1.0));
    rec.code("overrides", adm_monitor_set_overrides(nullptr, nullptr, 0U, 1U));
    rec.code("orientation", adm_monitor_set_listener_orientation(nullptr, 0.0F, 0.0F, 0.0F));
    rec.code("switch_backend", adm_monitor_switch_backend(nullptr, nullptr));
    rec.code("output_device", adm_monitor_set_output_device(nullptr, nullptr));
    rec.put("log_count", adm_monitor_log_count(nullptr));
    adm_monitor_status_t status{};
    rec.code("status", adm_monitor_get_status(nullptr, &status));
    adm_monitor_levels_t levels{};
    rec.code("levels", adm_monitor_get_levels(nullptr, &levels));
    std::int32_t level = 0;
    const char* module = nullptr;
    const char* message = nullptr;
    rec.put("log_entry", adm_monitor_log_entry(nullptr, 0U, &level, &module, &message));
    adm_hptf_info_t info{};
    info.struct_size = sizeof(info);
    rec.code("hptf", adm_monitor_set_hptf(nullptr, nullptr));
    rec.code("hptf_info", adm_monitor_get_hptf_info(nullptr, &info));
    rec.code("hptf_parameters", adm_monitor_set_hptf_parameters(nullptr, nullptr));
    rec.code("hptf_stereo", adm_monitor_set_hptf_stereo(nullptr, nullptr));
    rec.code("hptf_stereo_parameters", adm_monitor_set_hptf_stereo_parameters(nullptr, nullptr));
    adm_hptf_stereo_info_t stereo_info{};
    stereo_info.struct_size = sizeof(stereo_info);
    rec.code("hptf_stereo_info", adm_monitor_get_hptf_stereo_info(nullptr, &stereo_info));
    std::uint64_t request = 0U;
    char* error = nullptr;
    rec.code("switch_room", adm_monitor_switch_room(nullptr, nullptr, &request, &error));
    rec.put("switch_room.error", error);
    adm_free_string(error);
    char* json = nullptr;
    rec.code("room_status", adm_monitor_room_status_json(nullptr, &json));
    rec.code("room_source", adm_monitor_room_source_json(nullptr, 1U, &json));
    adm_destroy_monitor(nullptr);
    adm_monitor_t* monitor = nullptr;
    rec.code("create.null_path", adm_create_monitor(context.value, nullptr, nullptr, &monitor));
    rec.put("create.null_path.last_error", adm_context_last_error_message(context.value));
    adm_destroy_monitor(monitor);
    monitor = nullptr;
    rec.code("create_ex.null_path", adm_create_monitor_ex(context.value, nullptr, nullptr, nullptr, &monitor));
    rec.put("create_ex.null_path.last_error", adm_context_last_error_message(context.value));
    adm_destroy_monitor(monitor);
}

// Scene output and room controls belong to migration family E, including their error paths.
void family_scene_output(Recorder& rec, const Context& context) {
    rec.begin("scene_output.null");
    rec.put("last_error", adm_scene_output_last_error_message(nullptr));
    rec.code("play", adm_scene_output_play(nullptr));
    rec.code("pause", adm_scene_output_pause(nullptr));
    rec.code("volume", adm_scene_output_set_volume(nullptr, 1.0F));
    rec.code("begin_epoch", adm_scene_output_begin_epoch(nullptr, 1U, 0));
    adm_scene_output_status_t output_status{};
    output_status.struct_size = sizeof(output_status);
    rec.code("status", adm_scene_output_get_status(nullptr, &output_status));
    rec.code("hptf", adm_scene_output_set_hptf(nullptr, nullptr));
    adm_hptf_info_t info{};
    info.struct_size = sizeof(info);
    rec.code("hptf_info", adm_scene_output_get_hptf_info(nullptr, &info));
    rec.code("hptf_parameters", adm_scene_output_set_hptf_parameters(nullptr, nullptr));
    rec.code("hptf_stereo", adm_scene_output_set_hptf_stereo(nullptr, nullptr));
    rec.code("hptf_stereo_parameters", adm_scene_output_set_hptf_stereo_parameters(nullptr, nullptr));
    adm_hptf_stereo_info_t stereo_info{};
    stereo_info.struct_size = sizeof(stereo_info);
    rec.code("hptf_stereo_info", adm_scene_output_get_hptf_stereo_info(nullptr, &stereo_info));
    char* error = nullptr;
    rec.code("hptf_ex", adm_scene_output_set_hptf_ex(nullptr, nullptr, &error));
    rec.owned("hptf_ex.error", error);
    error = nullptr;
    rec.code("hptf_stereo_ex", adm_scene_output_set_hptf_stereo_ex(nullptr, nullptr, &error));
    rec.owned("hptf_stereo_ex.error", error);
    adm_destroy_scene_output(nullptr);
    adm_scene_output_t* output = nullptr;
    rec.code("create.null_stream", adm_create_scene_output(context.value, nullptr, nullptr, &output));
    adm_destroy_scene_output(output);

    rec.begin("scene_output.null_device");
    {
        adm_scene_stream_t* stream = nullptr;
        const auto config = stream_config(renderer_config(ADM_RENDERER_SAF_BINAURAL, "binaural"), 48000U);
        rec.code("stream", adm_create_scene_stream(context.value, &config, &stream));
        adm_scene_output_config_t output_config{};
        output_config.struct_size = sizeof(output_config);
        output_config.kind = ADM_SCENE_OUTPUT_NULL;
        output = nullptr;
        rec.code("create", adm_create_scene_output(context.value, stream, &output_config, &output));
        rec.put("create.last_error", adm_context_last_error_message(context.value));
        if (output != nullptr) {
            // Created paused: nothing below depends on a device clock.
            output_status = {};
            output_status.struct_size = sizeof(output_status);
            rec.code("status", adm_scene_output_get_status(output, &output_status));
            rec.put("status.paused",
                    std::to_string(output_status.state) + " epoch=" + std::to_string(output_status.epoch_id) +
                        " consumed=" + std::to_string(output_status.consumed_frames) +
                        " clock=" + std::to_string(output_status.clock_kind));
            rec.code("volume", adm_scene_output_set_volume(output, 0.5F));
            rec.code("volume.negative", adm_scene_output_set_volume(output, -1.0F));
            rec.put("volume.negative.last_error", adm_scene_output_last_error_message(output));
            const std::array<adm_hptf_band_t, 2> bands{
                {{sizeof(adm_hptf_band_t), ADM_HPTF_BAND_PEAKING, 1, 0U, 1000.0, -3.0, 1.0},
                 {sizeof(adm_hptf_band_t), ADM_HPTF_BAND_HIGH_SHELF, 1, 0U, 8000.0, 2.0, 0.71}}};
            adm_hptf_parameters_t parameters{};
            parameters.struct_size = sizeof(parameters);
            parameters.band_count = 2U;
            parameters.bands = bands.data();
            parameters.preamp_db = -2.0;
            parameters.preamp_mode = ADM_HPTF_PREAMP_AUTO_TRIM;
            parameters.revision = 3U;
            rec.code("hptf_parameters", adm_scene_output_set_hptf_parameters(output, &parameters));
            info = {};
            info.struct_size = sizeof(info);
            rec.code("hptf_info", adm_scene_output_get_hptf_info(output, &info));
            rec.put("hptf_info.value",
                    std::to_string(info.enabled) + ' ' + std::to_string(info.band_count) + ' ' +
                        std::to_string(info.sample_rate) + ' ' + bits(static_cast<double>(info.preamp_db)) + ' ' +
                        bits(static_cast<double>(info.auto_trim_db)) + ' ' +
                        bits(static_cast<double>(info.max_response_db)));
            parameters.band_count = 1U;
            parameters.preamp_db = std::numeric_limits<double>::infinity();
            rec.code("hptf_parameters.invalid", adm_scene_output_set_hptf_parameters(output, &parameters));
            rec.put("hptf_parameters.invalid.last_error", adm_scene_output_last_error_message(output));
            rec.code("begin_epoch", adm_scene_output_begin_epoch(output, 1U, 0));
        }
        adm_destroy_scene_output(output);
        std::uint64_t switch_request = 0U;
        error = nullptr;
        rec.code("stream.switch_room.not_room", adm_scene_stream_switch_room(stream, nullptr, &switch_request, &error));
        rec.owned("stream.switch_room.not_room.error", error);
        char* json = nullptr;
        rec.code("stream.room_status.not_room", adm_scene_stream_room_status_json(stream, &json));
        rec.owned("stream.room_status.not_room.json", json);
        adm_destroy_scene_stream(stream);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mr_adm_capi_capture <out-dir> [family...]\n";
        return 2;
    }
    const fs::path out = argv[1];
    std::set<std::string> families;
    for (int i = 2; i < argc; ++i) {
        families.insert(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    const auto selected = [&](const char* family) { return families.empty() || families.contains(family); };
    fs::create_directories(out);
    Recorder rec(out);
    if (!rec.ok()) {
        std::cerr << "cannot write " << (out / "capture.txt").string() << '\n';
        return 2;
    }
    const Context context;
    if (selected("base")) {
        family_base(rec);
    }
    if (selected("render")) {
        family_catalog(rec, context);
        family_probe(rec, context);
        family_inspect(rec, context);
        family_options(rec);
        family_render(rec, context);
        family_export(rec, context);
        family_room(rec, context);
    }
    if (selected("preview")) {
        family_preview(rec, context);
    }
    if (selected("scene")) {
        family_scene(rec, context);
        family_scene_output(rec, context);
    }
    if (selected("monitor")) {
        family_monitor(rec, context);
    }
    return 0;
}
