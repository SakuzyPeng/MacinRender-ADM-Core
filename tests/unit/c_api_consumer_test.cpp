#include "../support/adm_fixture.h"
// Consumer test for the stable C ABI (include/adm/c_api.h): the checks that need the C++ islands
// (encoders, output devices, the Apple backend and the platform facts) or a C++ caller, plus one
// smoke call per C ABI family. Everything else about the C ABI is tested in Rust, next to its
// implementation (`rust/crates/mradm-capi/src/tests`, R3 E). It also runs against the shared
// library for the same-build comparison of two C ABI implementations (capi-capture.py tests).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/c_api.h"

// libadm / libbw64 — used here only to construct the fixture, never via the ABI.
#include "../support/wav_fixture.h"

namespace {

class FileGuard {
  public:
    explicit FileGuard(std::filesystem::path path) : path_(std::move(path)) {}
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    FileGuard(FileGuard&&) = delete;
    FileGuard& operator=(FileGuard&&) = delete;
    ~FileGuard() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return condition;
}

std::filesystem::path unique_temp_wav_path(const char* stem) {
    static uint64_t counter = 0;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string{stem} + "_" + std::to_string(ticks) + "_" + std::to_string(counter++) + ".wav");
}

// Build a minimal single-Object ADM BW64 file with real audio samples.
std::filesystem::path write_fixture(uint32_t sample_rate = 48000U, bool cartesian = false) {
    auto doc = fixture::Document::create();

    auto cf = fixture::AudioChannelFormat::create(fixture::AudioChannelFormatName{"TestCF"},
                                                  fixture::TypeDefinition::objects);
    {
        auto block = cartesian ? fixture::AudioBlockFormatObjects{fixture::CartesianPosition{
                                     fixture::X{0.0F}, fixture::Y{1.0F}, fixture::Z{.5F}}}
                               : fixture::AudioBlockFormatObjects{
                                     fixture::SphericalPosition{fixture::Azimuth{30.0F}, fixture::Elevation{10.0F}}};
        if (cartesian) {
            block.set(fixture::Width{.3F});
            block.set(fixture::Height{.3F});
            block.set(fixture::Depth{.3F});
        }
        block.set(fixture::Gain{0.8F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf =
        fixture::AudioPackFormat::create(fixture::AudioPackFormatName{"TestPF"}, fixture::TypeDefinition::objects);
    pf->add_reference(cf);
    doc->add(pf);

    auto sf =
        fixture::AudioStreamFormat::create(fixture::AudioStreamFormatName{"TestSF"}, fixture::FormatDefinition::pcm);
    sf->set_reference(cf);
    doc->add(sf);

    auto tf =
        fixture::AudioTrackFormat::create(fixture::AudioTrackFormatName{"TestTF"}, fixture::FormatDefinition::pcm);
    tf->set_reference(sf);
    sf->add_reference(tf);
    doc->add(tf);

    auto uid = fixture::AudioTrackUid::create();
    uid->set_reference(tf);
    uid->set_reference(pf);
    doc->add(uid);

    auto object = fixture::AudioObject::create(fixture::AudioObjectName{"TestObject"});
    object->add_reference(uid);
    doc->add(object);

    auto content = fixture::AudioContent::create(fixture::AudioContentName{"TestContent"});
    content->add_reference(object);
    doc->add(content);

    auto programme = fixture::AudioProgramme::create(fixture::AudioProgrammeName{"TestProgramme"});
    programme->add_reference(content);
    doc->add(programme);

    fixture::reassign_ids(doc);
    const std::string uid_str = fixture::format_id(uid->get<fixture::AudioTrackUidId>());

    std::ostringstream buf;
    fixture::write_xml(buf, doc);

    auto path = unique_temp_wav_path("mr_c_api_fixture");
    auto chna = std::make_shared<fixture::WaveChna>(
        std::vector<fixture::WaveAudioId>{fixture::WaveAudioId(1, uid_str, "", "")});
    auto axml = std::make_shared<fixture::WaveAxml>(buf.str());
    auto writer = fixture::write_wave(path.string(), 1U, static_cast<uint16_t>(sample_rate), 24U, chna, axml);
    const uint64_t k_frames = static_cast<uint64_t>(sample_rate) / 5U; // 0.2 s
    std::vector<float> samples(k_frames, 0.5F);
    writer->write(samples.data(), k_frames);
    return path;
}

bool has_output_sidecar(const std::filesystem::path& final_path, const char* purpose) {
    const auto parent = final_path.parent_path();
    const auto prefix = final_path.stem().string() + "." + std::string{purpose} + ".";
    std::error_code ec;
    const std::filesystem::directory_iterator begin(parent, ec);
    if (ec) {
        return true;
    }
    return std::ranges::any_of(begin, std::filesystem::directory_iterator{}, [&](const auto& entry) {
        return entry.path().filename().string().starts_with(prefix);
    });
}

struct ProgressState {
    int calls{0};
    double last_fraction{-1.0};
    std::string last_stage;
};

void progress_cb(double fraction, const char* stage, const char* /*message*/, void* user_data) {
    auto* state = static_cast<ProgressState*>(user_data);
    ++state->calls;
    state->last_fraction = fraction;
    state->last_stage = (stage != nullptr) ? stage : "";
}

struct ProgressV2State {
    std::vector<adm_progress_event_v2_t> events;
    bool messages_non_null{true};
};

void progress_v2_cb(const adm_progress_event_v2_t* event, void* user_data) {
    auto* state = static_cast<ProgressV2State*>(user_data);
    if (event == nullptr) {
        state->messages_non_null = false;
        return;
    }
    state->messages_non_null = state->messages_non_null && event->message != nullptr;
    state->events.push_back(*event);
}

std::string owned(char* text) {
    std::string value = text != nullptr ? text : "";
    adm_free_string(text);
    return value;
}

// One call per C ABI family, through the library a C caller links: version and context, options
// and cancel tokens, a render to WAV with its result and v1 progress, a preview window, probe and
// inspect, a policy template, the discovery documents and an export.
bool verify_family_smoke(adm_context_t* ctx, const std::filesystem::path& input) {
    bool ok = check(adm_api_version_major() == ADM_API_VERSION_MAJOR && adm_api_version_minor() == ADM_API_VERSION_MINOR &&
                        adm_api_version_patch() == ADM_API_VERSION_PATCH,
                    "the library version matches the header");
    auto* opts = adm_create_render_options();
    auto* token = adm_create_cancel_token();
    ok = check(opts != nullptr && token != nullptr, "options and a cancel token are created") && ok;
    adm_render_options_set_cancel_token(opts, token);
    ok = check(adm_render_options_set_output_layout(opts, "5.1") == ADM_ERROR_OK &&
                   adm_render_options_set_renderer(opts, ADM_RENDERER_SAF) == ADM_ERROR_OK,
               "option setters accept a layout and a renderer") &&
         ok;

    const auto output = unique_temp_wav_path("mr_c_api_smoke");
    const FileGuard out_guard(output);
    ProgressState progress;
    adm_render_result_t* result = nullptr;
    const auto code = adm_render_file_ex(
        ctx, input.string().c_str(), output.string().c_str(), opts, &progress_cb, &progress, &result);
    ok = check(code == ADM_ERROR_OK && adm_render_result_error_code(result) == ADM_ERROR_OK,
               "a 5.1 WAV render succeeds") &&
         ok;
    ok = check(progress.calls > 0 && progress.last_stage == "finished" && progress.last_fraction >= 0.99,
               "v1 progress reaches finished") &&
         ok;
    ok = check(adm_render_result_log_count(result) > 0 && adm_render_result_output_path(result) != nullptr,
               "the result carries logs and the output path") &&
         ok;
    adm_destroy_render_result(result);
    adm_cancel(token);
    ok = check(adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, nullptr) ==
                   ADM_ERROR_CANCELLED,
               "a cancelled token stops the render") &&
         ok;
    adm_render_options_set_cancel_token(opts, nullptr);
    adm_destroy_cancel_token(token);

    adm_preview_session_t* session = nullptr;
    ok = check(adm_create_preview_session(ctx, input.string().c_str(), opts, &session) == ADM_ERROR_OK,
               "a preview session opens") &&
         ok;
    const auto window = unique_temp_wav_path("mr_c_api_smoke_window");
    const FileGuard window_guard(window);
    ok = check(adm_preview_render_window(session, 0.0, 0.1, window.string().c_str(), nullptr, nullptr, nullptr) ==
                   ADM_ERROR_OK,
               "a preview window renders") &&
         ok;
    adm_destroy_preview_session(session);
    adm_destroy_render_options(opts);

    adm_scene_info_t* info = nullptr;
    ok = check(adm_probe_file(ctx, input.string().c_str(), &info) == ADM_ERROR_OK &&
                   adm_scene_info_sample_rate(info) == 48000U && adm_scene_info_object_count(info) == 1U,
               "probe reads the fixture") &&
         ok;
    adm_destroy_scene_info(info);
    char* text = nullptr;
    ok = check(adm_inspect_file_json(ctx, input.string().c_str(), &text) == ADM_ERROR_OK &&
                   owned(text).find("TestObject") != std::string::npos,
               "inspect JSON names the object") &&
         ok;
    text = nullptr;
    ok = check(adm_inspect_file_xml(ctx, input.string().c_str(), &text) == ADM_ERROR_OK &&
                   owned(text).find("audioFormatExtended") != std::string::npos,
               "inspect XML returns the AXML") &&
         ok;
    text = nullptr;
    ok = check(adm_policy_template_json(ctx, input.string().c_str(), &text) == ADM_ERROR_OK &&
                   owned(text).find("mradm.semantic-policy.v1") != std::string::npos,
               "a policy template is written") &&
         ok;
    using Document = adm_error_code_t (*)(adm_context_t*, char**);
    for (const auto& [document, schema] : {std::pair<Document, const char*>{adm_capabilities_json, "mradm.capabilities"},
                                           {adm_layouts_json, "mradm.layouts"},
                                           {adm_input_layouts_json, "mradm.input-layouts"},
                                           {adm_output_formats_json, "mradm.output-formats"},
                                           {adm_render_support_matrix_json, "mradm.render-support-matrix"}}) {
        text = nullptr;
        ok = check(document(ctx, &text) == ADM_ERROR_OK && owned(text).find(schema) != std::string::npos,
                   "a discovery document carries its schema") &&
             ok;
    }
    const auto exported = unique_temp_wav_path("mr_c_api_smoke_export");
    const FileGuard export_guard(exported);
    ok = check(adm_export_file(ctx, input.string().c_str(), exported.string().c_str(), nullptr) == ADM_ERROR_OK &&
                   std::filesystem::exists(exported),
               "an ADM export is written") &&
         ok;
    return ok;
}

// The platform facts reach the discovery documents: APAC is available on macOS only.
bool verify_platform_documents(adm_context_t* ctx) {
    char* formats = nullptr;
    char* matrix = nullptr;
    bool ok = check(adm_output_formats_json(ctx, &formats) == ADM_ERROR_OK &&
                        adm_render_support_matrix_json(ctx, &matrix) == ADM_ERROR_OK,
                    "the output formats and the support matrix are written");
    const std::string f = owned(formats);
    const std::string m = owned(matrix);
#ifdef __APPLE__
    ok = check(f.find(R"("apac": true)") != std::string::npos && m.find(R"("apac": true)") != std::string::npos,
               "APAC is available on macOS") &&
         ok;
#else
    ok = check(f.find(R"("apac": false)") != std::string::npos &&
                   f.find(R"x("available_reason": "macOS only (AudioToolbox)")x") != std::string::npos &&
                   m.find(R"("apac": false)") != std::string::npos &&
                   m.find("macOS only (AudioToolbox)") != std::string::npos,
               "APAC is unavailable off macOS, with its reason") &&
         ok;
#endif
    return ok;
}

bool verify_lossy_encode_failure_cleanup(adm_context_t* ctx) {
    const FileGuard input_441(write_fixture(44100U));
    const auto out =
        std::filesystem::path(unique_temp_wav_path("mr_c_api_opus_fail_cleanup")).replace_extension(".mka");
    FileGuard out_guard(out);

    adm_render_options_t* opts = adm_create_render_options();
    bool ok = check(opts != nullptr, "cleanup failure options creation should succeed");
    if (opts == nullptr) {
        return false;
    }
    adm_render_options_set_renderer(opts, ADM_RENDERER_SAF);
    adm_render_options_set_output_layout(opts, "5.1");

    adm_render_result_t* r = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input_441.path().string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r);
    ok = check(code != ADM_ERROR_OK, "44.1 kHz Opus final encode should fail") && ok;
    ok = check(!std::filesystem::exists(out), "failed Opus final encode should leave no final output") && ok;
    ok =
        check(!has_output_sidecar(out, "render_tmp"), "failed Opus final encode should clean render_tmp sidecar") && ok;
    ok =
        check(!has_output_sidecar(out, "output_tmp"), "failed Opus final encode should clean output_tmp sidecar") && ok;

    adm_destroy_render_result(r);
    adm_destroy_render_options(opts);
    return ok;
}

bool verify_progress_v2_events(const ProgressV2State& state, bool require_post_processing, const char* label) {
    (void) label;
    bool ok = check(!state.events.empty(), "v2 progress: at least one event should fire");
    ok = check(state.messages_non_null, "v2 progress: all messages should be non-null") && ok;

    bool struct_size_ok = true;
    bool stage_ok = true;
    bool operation_ok = true;
    bool overall_ok = true;
    bool stage_fraction_ok = true;
    bool render_frames_ok = false;
    bool post_ok = !require_post_processing;
    double prev = -1.0;
    for (const auto& ev : state.events) {
        struct_size_ok = struct_size_ok && ev.struct_size == sizeof(adm_progress_event_v2_t);
        stage_ok = stage_ok && ev.stage != ADM_STAGE_UNKNOWN;
        operation_ok = operation_ok && ev.operation != ADM_PROGRESS_OPERATION_UNKNOWN;
        overall_ok =
            overall_ok && ev.overall_fraction >= prev && ev.overall_fraction >= 0.0 && ev.overall_fraction <= 1.0;
        stage_fraction_ok = stage_fraction_ok && ev.stage_fraction >= 0.0 && ev.stage_fraction <= 1.0;
        if (ev.operation == ADM_PROGRESS_OPERATION_RENDER_AUDIO && ev.total_frames > 0) {
            render_frames_ok = true;
        }
        if (ev.stage == ADM_STAGE_POST_PROCESSING) {
            post_ok = true;
        }
        prev = ev.overall_fraction;
    }
    ok = check(struct_size_ok, "v2 progress: struct_size should match") && ok;
    ok = check(stage_ok, "v2 progress: stage should be known") && ok;
    ok = check(operation_ok, "v2 progress: operation should be known") && ok;
    ok = check(overall_ok, "v2 progress: overall_fraction should be non-decreasing in [0,1]") && ok;
    ok = check(stage_fraction_ok, "v2 progress: stage_fraction should be in [0,1]") && ok;
    ok = check(render_frames_ok, "v2 progress: render events should carry total_frames") && ok;
    ok = check(post_ok, "v2 progress: should include post-processing when requested") && ok;
    if (!state.events.empty()) {
        const auto& last = state.events.back();
        ok = check(last.stage == ADM_STAGE_FINISHED, "v2 progress: last stage should be FINISHED") && ok;
        ok = check(last.operation == ADM_PROGRESS_OPERATION_FINISH, "v2 progress: last operation should be FINISH") &&
             ok;
        ok = check(last.overall_fraction >= 0.99, "v2 progress: final progress should reach ~1.0") && ok;
    }
    return ok;
}

bool verify_progress_v2_flac_post(adm_context_t* ctx, const std::filesystem::path& input) {
    auto output = unique_temp_wav_path("mr_c_api_prog_v2_flac");
    output.replace_extension(".flac");
    FileGuard guard(output);

    ProgressV2State state;
    const adm_error_code_t code = adm_render_file_ex2(
        ctx, input.string().c_str(), output.string().c_str(), nullptr, &progress_v2_cb, &state, nullptr);
    bool ok = check(code == ADM_ERROR_OK, "v2 progress FLAC render should succeed");
    ok = verify_progress_v2_events(state, true, "v2 progress FLAC") && ok;
    const bool has_flac = std::ranges::any_of(state.events, [](const adm_progress_event_v2_t& ev) {
        return ev.operation == ADM_PROGRESS_OPERATION_ENCODE_FLAC && ev.stage == ADM_STAGE_POST_PROCESSING;
    });
    ok = check(has_flac, "v2 progress FLAC should expose ENCODE_FLAC post-processing operation") && ok;
    return ok;
}

#ifdef __APPLE__
// The Apple backend runs inside the engine's step: a progress callback exception during its render
// still reaches the caller as INTERNAL with a null result, and a preview session reuses the Apple
// preparation across windows.
bool verify_apple_islands(adm_context_t* ctx, const std::filesystem::path& input) {
    auto* opts = adm_create_render_options();
    if (opts == nullptr) {
        return check(false, "Apple island options creation");
    }
    adm_render_options_set_renderer(opts, ADM_RENDERER_APPLE);
    adm_render_options_set_output_layout(opts, "binaural");
    bool ok = true;
    // WAV writes directly to its destination. FLAC stages the render before installing it.
    for (const auto& [extension, existing_output] :
         {std::pair{".wav", false}, std::pair{".flac", false}, std::pair{".flac", true}}) {
        auto output = unique_temp_wav_path("mr_c_api_apple_callback_failure");
        output.replace_extension(extension);
        const FileGuard guard(output);
        if (existing_output) {
            std::ofstream previous(output, std::ios::binary);
            previous << "previous output";
        }
        bool triggered = false;
        const auto callback = [](const adm_progress_event_v2_t* event, void* user) {
            if (event->operation == ADM_PROGRESS_OPERATION_RENDER_AUDIO && event->stage_fraction > 0.0) {
                *static_cast<bool*>(user) = true;
                throw std::runtime_error("Apple progress callback failed");
            }
        };
        adm_render_result_t* result = nullptr;
        const auto code = adm_render_file_ex2(
            ctx, input.string().c_str(), output.string().c_str(), opts, callback, &triggered, &result);
        ok = check(triggered && code == ADM_ERROR_INTERNAL && result == nullptr,
                   "an Apple render callback exception reaches the C ABI caller") &&
             ok;
        if (existing_output) {
            std::ifstream previous(output, std::ios::binary);
            const std::string content{std::istreambuf_iterator<char>{previous}, std::istreambuf_iterator<char>{}};
            ok = check(content == "previous output", "an Apple callback failure preserves the previous output") && ok;
        } else {
            ok = check(!std::filesystem::exists(output), "an Apple callback failure leaves no final output") && ok;
        }
        ok = check(!has_output_sidecar(output, "render_tmp") && !has_output_sidecar(output, "output_tmp"),
                   "an Apple callback failure cleans the staging files") &&
             ok;
        adm_destroy_render_result(result);
    }
    adm_preview_session_t* session = nullptr;
    ok = check(adm_create_preview_session(ctx, input.string().c_str(), opts, &session) == ADM_ERROR_OK &&
                   session != nullptr,
               "an Apple preview session opens") &&
         ok;
    for (const auto& [start, end] : {std::pair{0.0, 0.1}, std::pair{0.05, 0.15}}) {
        const auto output = unique_temp_wav_path("mr_c_api_apple_preview");
        const FileGuard guard(output);
        adm_render_result_t* result = nullptr;
        const auto code =
            adm_preview_render_window_v2(session, start, end, output.string().c_str(), nullptr, nullptr, &result);
        ok = check(code == ADM_ERROR_OK && std::filesystem::exists(output), "an Apple preview window renders") && ok;
        adm_destroy_render_result(result);
    }
    adm_destroy_preview_session(session);
    adm_destroy_render_options(opts);
    return ok;
}
#endif

bool verify_flac_callback_failures(adm_context_t* ctx, const std::filesystem::path& input) {
    enum class Action : uint8_t { cancel_at_end, throw_at_start, throw_at_end };
    struct State {
        adm_cancel_token_t* token{nullptr};
        Action action{Action::cancel_at_end};
        bool triggered{false};
    };
    const auto callback = [](const adm_progress_event_v2_t* event, void* user) {
        auto& state = *static_cast<State*>(user);
        if (event->operation != ADM_PROGRESS_OPERATION_ENCODE_FLAC ||
            (state.action != Action::throw_at_start && std::string_view{event->message} != "FLAC encoded")) {
            return;
        }
        state.triggered = true;
        if (state.action == Action::cancel_at_end) {
            adm_cancel(state.token);
        } else {
            throw std::runtime_error("FLAC progress callback failed");
        }
    };
    auto* opts = adm_create_render_options();
    auto* token = adm_create_cancel_token();
    if (opts == nullptr || token == nullptr) {
        adm_destroy_render_options(opts);
        adm_destroy_cancel_token(token);
        return check(false, "FLAC callback failure options and token creation");
    }
    adm_render_options_set_renderer(opts, ADM_RENDERER_SAF);
    adm_render_options_set_output_layout(opts, "5.1");
    adm_render_options_set_cancel_token(opts, token);
    bool ok = true;
    for (const auto action : {Action::cancel_at_end, Action::throw_at_start, Action::throw_at_end}) {
        for (const bool existing_output : {false, true}) {
            auto output = unique_temp_wav_path("mr_c_api_flac_callback_failure");
            output.replace_extension(".flac");
            const FileGuard guard(output);
            if (existing_output) {
                std::ofstream previous(output, std::ios::binary);
                previous << "previous output";
            }
            adm_reset_cancel_token(token);
            State state{token, action};
            adm_render_result_t* result = nullptr;
            const auto code = adm_render_file_ex2(
                ctx, input.string().c_str(), output.string().c_str(), opts, callback, &state, &result);
            const auto expected = action == Action::cancel_at_end ? ADM_ERROR_CANCELLED : ADM_ERROR_INTERNAL;
            ok = check(state.triggered && code == expected, "FLAC callback failure reaches the C ABI caller") && ok;
            if (action != Action::cancel_at_end) {
                ok = check(result == nullptr, "FLAC callback exception preserves the C ABI null result") && ok;
            }
            if (existing_output) {
                std::ifstream previous(output, std::ios::binary);
                const std::string content{std::istreambuf_iterator<char>{previous}, std::istreambuf_iterator<char>{}};
                ok = check(content == "previous output", "FLAC callback failure preserves the previous output") && ok;
            } else {
                ok = check(!std::filesystem::exists(output), "FLAC callback failure leaves no final output") && ok;
            }
            ok = check(!has_output_sidecar(output, "render_tmp") && !has_output_sidecar(output, "output_tmp"),
                       "FLAC callback failure cleans both staging files") &&
                 ok;
            adm_destroy_render_result(result);
        }
    }
    adm_destroy_render_options(opts);
    adm_destroy_cancel_token(token);
    return ok;
}

bool verify_opus_render(adm_context_t* ctx, const std::filesystem::path& input) {
    auto output = unique_temp_wav_path("mr_c_api_opus_51");
    output.replace_extension(".mka");
    FileGuard guard(output);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_output_layout(opts, "5.1");

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);

    bool ok = check(code == ADM_ERROR_OK, "Opus 5.1 MKA render should succeed");
    ok = check(std::filesystem::exists(output), "MKA output file should exist on disk") && ok;
    ok = check(std::filesystem::file_size(output) > 0, "MKA output file should be non-empty") && ok;
    if (result != nullptr) {
        const char* out_path = adm_render_result_output_path(result);
        ok = check(out_path != nullptr, "result output_path should be non-null for MKA render") && ok;
        if (out_path != nullptr) {
            ok = check(std::filesystem::path{out_path}.extension() == ".mka",
                       "result output_path should have .mka extension") &&
                 ok;
        }
    }
    adm_destroy_render_result(result);
    return ok;
}

bool verify_apac_unsupported_smoke(adm_context_t* ctx, const std::filesystem::path& input) {
    // On non-Apple platforms apac_io always returns UNSUPPORTED.
    // On Apple with AudioToolbox the render may succeed; we accept both.
    // Key invariants regardless of platform: no crash, result handle always allocated and destroyable.
    auto output = unique_temp_wav_path("mr_c_api_apac_smoke");
    output.replace_extension(".m4a");
    FileGuard guard(output); // harmless remove if file was never created

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), nullptr, nullptr, nullptr, &result);

    bool ok = check(result != nullptr, "APAC smoke: result must be allocated");
#ifdef __APPLE__
    ok = check(code == ADM_ERROR_OK || code == ADM_ERROR_UNSUPPORTED,
               "APAC smoke (Apple): should return OK or UNSUPPORTED") &&
         ok;
    if (code == ADM_ERROR_OK) {
        ok = check(std::filesystem::exists(output), "APAC smoke (Apple OK): .m4a file should exist") && ok;
        ok =
            check(std::filesystem::file_size(output) > 0, "APAC smoke (Apple OK): .m4a file should be non-empty") && ok;
    }
#else
    ok = check(code == ADM_ERROR_UNSUPPORTED, "APAC smoke (non-Apple): must return UNSUPPORTED") && ok;
#endif
    adm_destroy_render_result(result);
    return ok;
}

#ifdef __APPLE__
bool file_contains_bytes(const std::filesystem::path& path, std::string_view needle) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !std::ranges::search(bytes, needle).empty();
}
#endif

bool verify_apac_caf_smoke(adm_context_t* ctx, const std::filesystem::path& input) {
    auto output = unique_temp_wav_path("mr_c_api_apac_caf_smoke");
    output.replace_extension(".caf");
    FileGuard guard(output); // harmless remove if file was never created

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_apac_container(opts, ADM_APAC_CONTAINER_CAF);

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);

    bool ok = check(result != nullptr, "APAC CAF smoke: result must be allocated");
#ifdef __APPLE__
    ok = check(code == ADM_ERROR_OK || code == ADM_ERROR_UNSUPPORTED,
               "APAC CAF smoke (Apple): should return OK or UNSUPPORTED") &&
         ok;
    if (code == ADM_ERROR_OK) {
        ok = check(std::filesystem::exists(output), "APAC CAF smoke (Apple OK): .caf file should exist") && ok;
        ok =
            check(std::filesystem::file_size(output) > 0, "APAC CAF smoke (Apple OK): .caf file should be non-empty") &&
            ok;
        ok = check(file_contains_bytes(output, "caff"), "APAC CAF smoke: output contains CAF magic") && ok;
        ok = check(file_contains_bytes(output, "apac"), "APAC CAF smoke: output contains apac format id") && ok;
    }
#else
    ok = check(code == ADM_ERROR_UNSUPPORTED, "APAC CAF smoke (non-Apple): must return UNSUPPORTED") && ok;
#endif
    adm_destroy_render_result(result);
    return ok;
}

bool verify_iamf_smoke(adm_context_t* ctx, const std::filesystem::path& input) {
    // When MR_ADM_ENABLE_IAMF=OFF (default CI): .iamf output must return UNSUPPORTED.
    // When MR_ADM_ENABLE_IAMF=ON: render with a supported layout and expect success.
    auto output = unique_temp_wav_path("mr_c_api_iamf_smoke");
    output.replace_extension(".iamf");
    FileGuard guard(output);

#if MR_ADM_ENABLE_IAMF
    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_output_layout(opts, "5.1");
    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);
    bool ok = check(result != nullptr, "IAMF smoke (enabled): result must be allocated");
    ok = check(code == ADM_ERROR_OK, "IAMF smoke (enabled): render should succeed") && ok;
    ok = check(std::filesystem::exists(output), "IAMF smoke (enabled): output file should exist") && ok;
    ok = check(std::filesystem::file_size(output) > 0, "IAMF smoke (enabled): output file should be non-empty") && ok;
#else
    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), nullptr, nullptr, nullptr, &result);
    bool ok = check(result != nullptr, "IAMF smoke (disabled): result must be allocated");
    ok = check(code == ADM_ERROR_UNSUPPORTED, "IAMF smoke (disabled): must return UNSUPPORTED") && ok;
#endif
    adm_destroy_render_result(result);
    return ok;
}

} // namespace

bool configure_monitor_hptf_memory(adm_monitor_t* monitor) {
    std::array<adm_hptf_band_t, 1> bands{{{sizeof(adm_hptf_band_t), ADM_HPTF_BAND_PEAKING, 1, 0, 1000.0, 3.0, 1.5}}};
    const adm_hptf_parameters_t parameters{
        sizeof(adm_hptf_parameters_t), 1, bands.data(), -9.0, ADM_HPTF_PREAMP_WARN_ONLY, 0, 13801};
    bool ok = check(adm_monitor_set_hptf_parameters(monitor, &parameters) == ADM_ERROR_OK,
                    "monitor accepts an in-memory HpTF profile");
    // cppcheck-suppress unreadVariable; the C ABI reads this through parameters.bands.
    bands[0].q = std::numeric_limits<double>::quiet_NaN();
    ok = check(adm_monitor_set_hptf_parameters(monitor, &parameters) == ADM_ERROR_INVALID_ARGUMENT,
               "invalid editor draft preserves the accepted monitor profile") &&
         ok;
    // All caller-owned storage goes away here, before playback and device/engine rebuilding.
    return ok;
}

bool verify_monitor_hptf_memory_applied(adm_monitor_t* monitor) {
    adm_hptf_info_t info{};
    info.struct_size = sizeof(info);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (adm_monitor_get_hptf_info(monitor, &info) != ADM_ERROR_OK) {
            return check(false, "monitor HpTF status failed");
        }
        if (info.applied_revision == 13801) {
            return check(info.enabled == 1 && info.band_count == 1 && info.preamp_db == -9.0F,
                         "monitor rebuild preserves owned HpTF parameters and revision");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    return check(false, "monitor memory HpTF did not become active");
}

bool verify_triple_balance_monitor_abi(adm_context_t* ctx) {
    const auto input = write_fixture(48000, true);
    const FileGuard guard{input};
    auto* options = adm_create_render_options();
    if (options == nullptr) {
        return false;
    }
    adm_render_options_set_renderer(options, ADM_RENDERER_TRIPLE_BALANCE);
    adm_render_options_set_output_layout(options, "7.1.4");
    adm_monitor_t* monitor = nullptr;
    const auto created = adm_create_monitor(ctx, input.string().c_str(), options, &monitor);
    adm_destroy_render_options(options);
    if (created != ADM_ERROR_OK) {
        const std::string message = adm_context_last_error_message(ctx);
        std::cout << "Triple Balance monitor device unavailable: " << message << '\n';
        return check(created != ADM_ERROR_UNSUPPORTED ||
                         message.find("multichannel monitoring requires a multichannel device") != std::string::npos,
                     "Triple Balance stream is supported but raw stereo devices are rejected");
    }
    adm_monitor_override_t edit{};
    edit.struct_size = sizeof(edit);
    edit.object_id = "AO_1001";
    edit.diffuse_scale = edit.divergence_scale = 1;
    edit.extent_scale = .5F;
    edit.extent_width_scale = edit.extent_height_scale = edit.extent_depth_scale = 1;
    bool ok = check(adm_monitor_set_overrides(monitor, &edit, 1, 101) == ADM_ERROR_OK,
                    "Triple Balance accepts linked size multiplier");
    edit.extent_height_scale = 2;
    ok &= check(adm_monitor_set_overrides(monitor, &edit, 1, 102) == ADM_ERROR_UNSUPPORTED,
                "Triple Balance rejects unlinked size through C ABI");
    ok &= check(std::strlen(adm_monitor_last_error_message(monitor)) > 0, "rejected live edit explains the error");
    adm_monitor_status_t status{};
    status.struct_size = sizeof(status);
    adm_monitor_get_status(monitor, &status);
    ok &= check(status.override_revision != 102, "rejected C ABI revision is not acknowledged");
    adm_destroy_monitor(monitor);
    return ok;
}

// v1.15: realtime monitor. Tolerant of headless CI with no audio output device — the
// create may fail with a device error, in which case the playback assertions are skipped.
// NOLINTNEXTLINE(readability-function-size): versioned ABI validation shares one session and cleanup.
bool verify_monitor_abi(adm_context_t* ctx, const std::filesystem::path& input) {
    bool ok = check(adm_api_version_minor() == ADM_API_VERSION_MINOR, "C ABI minor version matches header");
    ok = check(adm_api_version_minor() >= 29, "v1.29: monitor mute override is available") && ok;
    ok = check(adm_api_version_minor() >= 30, "v1.30: 22.2 LFE routing mode is available") && ok;
    ok = check(adm_api_version_minor() >= 31, "v1.31: selectable speaker geometry is available") && ok;
    ok = check(adm_api_version_minor() >= 32, "v1.32: DirectSpeakers routing mode is available") && ok;
    ok = check(adm_api_version_minor() >= 33, "v1.33: optional head_locked override is available") && ok;
    ok = check(adm_api_version_minor() >= 34, "v1.34: DirectSpeakers matrix routing is available") && ok;
    static_assert(offsetof(adm_monitor_override_t, mute) >=
                  offsetof(adm_monitor_override_t, head_locked) + sizeof(int32_t) + sizeof(uint32_t));
    static_assert(offsetof(adm_monitor_override_t, reserved_v1_33) ==
                  offsetof(adm_monitor_override_t, mute) + sizeof(int32_t));
    static_assert(offsetof(adm_monitor_override_t, head_locked_valid) ==
                  offsetof(adm_monitor_override_t, reserved_v1_33) + sizeof(uint32_t));
    static_assert(sizeof(adm_monitor_override_t) >=
                  offsetof(adm_monitor_override_t, head_locked_valid) + sizeof(int32_t));

    ok = check(adm_api_version_minor() >= 37, "v1.37: HpTF headphone compensation is available") && ok;
    // The v1.37 tail-padding guard: reserved_v1_37 must sit immediately after preamp_mode so a
    // future appended field never lands inside a previous caller's trailing alignment padding.
    static_assert(offsetof(adm_hptf_config_t, reserved_v1_37) ==
                  offsetof(adm_hptf_config_t, preamp_mode) + sizeof(int32_t));
    static_assert(offsetof(adm_hptf_config_t, revision) ==
                  offsetof(adm_hptf_config_t, reserved_v1_37) + sizeof(uint32_t));
    static_assert(sizeof(adm_hptf_config_t) >= offsetof(adm_hptf_config_t, revision) + sizeof(uint64_t));
    static_assert(offsetof(adm_hptf_info_t, applied_revision) ==
                  offsetof(adm_hptf_info_t, reserved_v1_37) + sizeof(uint32_t));

    // v1.21 device enumeration (headless returns an empty JSON array).
    char* dev_json = nullptr;
    ok = check(adm_monitor_output_devices_json(ctx, &dev_json) == ADM_ERROR_OK, "output_devices_json ok") && ok;
    ok = check(dev_json != nullptr && dev_json[0] == '[', "output_devices_json returns a JSON array") && ok;
    adm_free_string(dev_json);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_renderer(opts, ADM_RENDERER_SAF_BINAURAL);
    adm_render_options_set_output_layout(opts, "binaural");
    adm_monitor_t* monitor = nullptr;
    const adm_error_code_t code = adm_create_monitor(ctx, input.string().c_str(), opts, &monitor);
    adm_destroy_render_options(opts);

    if (code != ADM_ERROR_OK) {
        // No audio output device available (headless CI): must fail cleanly.
        ok = check(monitor == nullptr, "failed create_monitor leaves out null") && ok;
        std::cerr << "(no audio output device; skipping monitor playback assertions)\n";
        return ok;
    }
    ok = check(monitor != nullptr, "create_monitor returns a monitor") && ok;
    ok = configure_monitor_hptf_memory(monitor) && ok;

    ok = check(adm_monitor_play(monitor) == ADM_ERROR_OK, "monitor play") && ok;

    adm_monitor_status_t status{};
    status.struct_size = sizeof(status);
    ok = check(adm_monitor_get_status(monitor, &status) == ADM_ERROR_OK, "monitor get_status") && ok;

    std::vector<float> peak(8U, 0.0F);
    std::vector<float> rms(8U, 0.0F);
    adm_monitor_levels_t levels{};
    levels.struct_size = sizeof(levels);
    levels.capacity = static_cast<uint32_t>(peak.size());
    levels.peak = peak.data();
    levels.rms = rms.data();
    ok = check(adm_monitor_get_levels(monitor, &levels) == ADM_ERROR_OK, "monitor get_levels") && ok;
    ok = check(levels.out_count >= 1U, "monitor levels report a channel count") && ok;
    // v1.18 LUFS: present in the struct and either a finite value or the -inf silence
    // sentinel (no playback may have happened yet on a headless runner).
    ok = check(!std::isnan(levels.momentary_lufs) && !std::isnan(levels.integrated_lufs),
               "monitor levels LUFS fields are populated (finite or -inf, not NaN)") &&
         ok;

    ok = check(adm_monitor_seek(monitor, 0.0) == ADM_ERROR_OK, "monitor seek") && ok;
    ok = check(adm_monitor_set_loop(monitor, 0.0, 0.5) == ADM_ERROR_OK, "monitor set_loop") && ok;
    ok = verify_monitor_hptf_memory_applied(monitor) && ok;

    // Clearing overrides (NULL + count 0) is valid; a populated set echoes its revision.
    ok = check(adm_monitor_set_overrides(monitor, nullptr, 0, 0) == ADM_ERROR_OK, "monitor clear overrides") && ok;
    adm_monitor_override_t ov{};
    ov.struct_size = sizeof(adm_monitor_override_t);
    ov.object_id = "AO_1001";
    ov.gain_db = -6.0F;
    ov.diffuse_scale = 1.0F;
    ov.extent_scale = 1.0F;
    ov.divergence_scale = 1.0F;
    ov.extent_width_scale = 1.0F;
    ov.extent_height_scale = 1.0F;
    ov.extent_depth_scale = 1.0F;
    ov.speaker_label = "M+030"; // v1.20: per-channel DirectSpeakers filter (whole-object when NULL)
    ov.head_locked = 1;         // v1.23: head-locked (excluded from head tracking)
    ov.reserved_v1_29 = 0;
    ov.mute = 1; // v1.29: exact per-channel mute; gain_db is ignored
    ov.reserved_v1_33 = 0;
    ov.head_locked_valid = 1; // v1.33: explicitly apply head_locked
    ok = check(adm_monitor_set_overrides(monitor, &ov, 1, 7) == ADM_ERROR_OK, "monitor set overrides") && ok;
    adm_monitor_override_t legacy = ov;
    legacy.struct_size = static_cast<uint32_t>(offsetof(adm_monitor_override_t, extent_width_scale));
    legacy.extent_width_scale = 0.0F;
    legacy.extent_height_scale = 0.0F;
    legacy.extent_depth_scale = 0.0F;
    ok = check(adm_monitor_set_overrides(monitor, &legacy, 1, 8) == ADM_ERROR_OK,
               "monitor set_overrides accepts legacy struct_size without extent axis fields") &&
         ok;
    // A struct_size that stops before speaker_label must be accepted (field ignored → whole-object),
    // even when the bytes past struct_size hold a non-NULL pointer the library must not read.
    adm_monitor_override_t pre_label = ov;
    pre_label.struct_size = static_cast<uint32_t>(offsetof(adm_monitor_override_t, speaker_label));
    ok = check(adm_monitor_set_overrides(monitor, &pre_label, 1, 8) == ADM_ERROR_OK,
               "monitor set_overrides accepts legacy struct_size without speaker_label field") &&
         ok;
    // A v1.23-v1.28 caller stops before the v1.29 mute field. The intervening size guard keeps
    // mute beyond the old 64-bit sizeof even though head_locked previously had tail padding.
    adm_monitor_override_t pre_mute = ov;
    pre_mute.struct_size = static_cast<uint32_t>(offsetof(adm_monitor_override_t, mute));
    ok = check(adm_monitor_set_overrides(monitor, &pre_mute, 1, 8) == ADM_ERROR_OK,
               "monitor set_overrides accepts legacy struct_size without mute field") &&
         ok;
    // v1.29-v1.32 callers end at the old 72-byte size and therefore do not carry
    // head_locked_valid. They retain the v1.23 always-explicit head_locked behavior.
    adm_monitor_override_t pre_valid = ov;
    pre_valid.struct_size = static_cast<uint32_t>(offsetof(adm_monitor_override_t, head_locked_valid));
    ok = check(adm_monitor_set_overrides(monitor, &pre_valid, 1, 8) == ADM_ERROR_OK,
               "monitor set_overrides accepts v1.29-v1.32 size without head_locked_valid") &&
         ok;
    // A full v1.33 entry with valid=0 is gain/mute-only and inherits ADM headLocked.
    adm_monitor_override_t inherit_head = ov;
    inherit_head.head_locked_valid = 0;
    ok = check(adm_monitor_set_overrides(monitor, &inherit_head, 1, 8) == ADM_ERROR_OK,
               "monitor set_overrides accepts v1.33 inherited head_locked") &&
         ok;

    // v1.22 listener orientation: a finite yaw/pitch/roll is accepted on the live monitor (the
    // SAF binaural backend ignores it, but the call must still succeed); NaN is rejected.
    ok = check(adm_monitor_set_listener_orientation(monitor, 30.0F, -10.0F, 0.0F) == ADM_ERROR_OK,
               "monitor set_listener_orientation accepts finite angles") &&
         ok;
    ok = check(adm_monitor_set_listener_orientation(monitor, std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_listener_orientation rejects NaN yaw") &&
         ok;

    // Non-finite gain / scale is rejected; struct_size below the minimum is rejected.
    adm_monitor_override_t bad = ov;
    bad.gain_db = std::numeric_limits<float>::quiet_NaN();
    ok = check(adm_monitor_set_overrides(monitor, &bad, 1, 9) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects NaN gain") &&
         ok;
    bad = ov;
    bad.gain_db = 800.0F;
    bad.speaker_label = nullptr;
    bad.mute = 0;
    ok = check(adm_monitor_set_overrides(monitor, &bad, 1, 9) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects finite dB that overflows linear gain") &&
         ok;
    ok = check(std::strlen(adm_monitor_last_error_message(monitor)) > 0,
               "monitor gain overflow rejection includes a diagnostic") &&
         ok;
    // Legacy callers have no mute field; trailing caller bytes must not bypass validation.
    bad.struct_size = static_cast<uint32_t>(offsetof(adm_monitor_override_t, mute));
    bad.mute = 1;
    ok = check(adm_monitor_set_overrides(monitor, &bad, 1, 9) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects overflowing gain from callers without mute") &&
         ok;
    bad = ov;
    bad.extent_width_scale = std::numeric_limits<float>::quiet_NaN();
    ok = check(adm_monitor_set_overrides(monitor, &bad, 1, 10) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects NaN extent width scale") &&
         ok;
    adm_monitor_override_t small = ov;
    small.struct_size = 4U; // below the minimum (must cover through divergence_scale)
    ok = check(adm_monitor_set_overrides(monitor, &small, 1, 11) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects undersized struct_size") &&
         ok;

    // Forward-compat stride: simulate a caller built against a LARGER (future) struct by
    // advertising struct_size > sizeof and laying elements out at that stride. The library
    // must advance by the caller's struct_size to find element[1], not by its own sizeof.
    {
        const std::size_t fut_stride = sizeof(adm_monitor_override_t) + 16U; // pretend future appended fields
        std::vector<unsigned char> raw(fut_stride * 2U, 0U);
        auto place = [&](std::size_t idx, const char* id, float gain) {
            adm_monitor_override_t e{};
            e.struct_size = static_cast<uint32_t>(fut_stride);
            e.object_id = id;
            e.gain_db = gain;
            e.diffuse_scale = 1.0F;
            e.extent_scale = 1.0F;
            e.divergence_scale = 1.0F;
            e.extent_width_scale = 1.0F;
            e.extent_height_scale = 1.0F;
            e.extent_depth_scale = 1.0F;
            std::memcpy(raw.data() + (idx * fut_stride), &e, sizeof(e));
        };
        // Positive control: both elements valid at the future stride → accepted.
        place(0, "AO_1001", -3.0F);
        place(1, "AO_1002", -6.0F);
        const auto* arr = reinterpret_cast<const adm_monitor_override_t*>(raw.data());
        ok = check(adm_monitor_set_overrides(monitor, arr, 2, 12) == ADM_ERROR_OK,
                   "monitor set_overrides accepts future (oversized) struct_size stride") &&
             ok;
        // Poison ONLY element[1]'s gain. A correct stride reads it (NaN → reject); a buggy
        // stride (== sizeof) would read element[1] from element[0]'s zero padding and miss it.
        place(1, "AO_1002", std::numeric_limits<float>::quiet_NaN());
        ok = check(adm_monitor_set_overrides(monitor, arr, 2, 13) == ADM_ERROR_INVALID_ARGUMENT,
                   "monitor set_overrides strides by caller struct_size to reach element[1]") &&
             ok;
    }

    // Same-format hot-switch (binaural → binaural) crossfades in and succeeds.
    adm_render_options_t* sw_same = adm_create_render_options();
    adm_render_options_set_renderer(sw_same, ADM_RENDERER_SAF_BINAURAL);
    adm_render_options_set_output_layout(sw_same, "binaural");
    ok =
        check(adm_monitor_switch_backend(monitor, sw_same) == ADM_ERROR_OK, "monitor same-format backend switch") && ok;
    adm_destroy_render_options(sw_same);

    // A speaker or HOA stream must not replace binaural output with a plain stereo fold.
    const auto reject_speaker_switch = [&](adm_renderer_t backend, const char* layout) {
        auto* sw_diff = adm_create_render_options();
        adm_render_options_set_renderer(sw_diff, backend);
        adm_render_options_set_output_layout(sw_diff, layout);
        const auto rejected = adm_monitor_switch_backend(monitor, sw_diff);
        adm_destroy_render_options(sw_diff);
        return check(rejected == ADM_ERROR_UNSUPPORTED &&
                         std::string_view{adm_monitor_last_error_message(monitor)}.find("channel count") !=
                             std::string_view::npos,
                     "monitor rejects speaker/HOA downmix to binaural output");
    };
    ok &= reject_speaker_switch(ADM_RENDERER_EAR, "0+5+0");
    ok &= reject_speaker_switch(ADM_RENDERER_SAF, "0+5+0");
    ok &= reject_speaker_switch(ADM_RENDERER_HOA, "hoa3");
#ifdef __APPLE__
    ok &= reject_speaker_switch(ADM_RENDERER_APPLE, "0+5+0");
#endif
    ok &= check(adm_monitor_get_status(monitor, &status) == ADM_ERROR_OK && status.failed == 0 &&
                    adm_monitor_get_levels(monitor, &levels) == ADM_ERROR_OK && levels.out_count == 2U,
                "rejected cross-format switches preserve the working binaural monitor");

    // v1.21 live output-device switch: "" = default device (no-op, monitor opened on default);
    // a bogus token rebuilds on the default device (graceful fallback) rather than failing.
    ok = check(adm_monitor_set_output_device(monitor, "") == ADM_ERROR_OK, "set_output_device default no-op") && ok;
    ok = check(adm_monitor_set_output_device(monitor, "deadbeef") == ADM_ERROR_OK,
               "set_output_device bogus token falls back to default device") &&
         ok;
    ok = verify_monitor_hptf_memory_applied(monitor) && ok;

    ok = check(adm_monitor_pause(monitor) == ADM_ERROR_OK, "monitor pause") && ok;
    ok = check(adm_monitor_log_entry(monitor, adm_monitor_log_count(monitor) + 100U, nullptr, nullptr, nullptr) == 0,
               "monitor log_entry out-of-range is 0") &&
         ok;

    adm_destroy_monitor(monitor);
    return ok;
}

int main() try {
    adm_context_t* ctx = adm_create_context();
    if (!check(ctx != nullptr, "context creation should succeed")) {
        return EXIT_FAILURE;
    }
    const FileGuard fixture(write_fixture());
    bool ok = verify_family_smoke(ctx, fixture.path());
    ok = verify_platform_documents(ctx) && ok;
    ok = verify_lossy_encode_failure_cleanup(ctx) && ok;
    ok = verify_progress_v2_flac_post(ctx, fixture.path()) && ok;
    ok = verify_flac_callback_failures(ctx, fixture.path()) && ok;
    ok = verify_opus_render(ctx, fixture.path()) && ok;
#ifdef __APPLE__
    ok = verify_apple_islands(ctx, fixture.path()) && ok;
#endif
    ok = verify_apac_unsupported_smoke(ctx, fixture.path()) && ok;
    ok = verify_apac_caf_smoke(ctx, fixture.path()) && ok;
    ok = verify_iamf_smoke(ctx, fixture.path()) && ok;
    ok = verify_triple_balance_monitor_abi(ctx) && ok;
    ok = verify_monitor_abi(ctx, fixture.path()) && ok;

    adm_destroy_context(ctx);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception& error) {
    std::cerr << "fixture setup failed: " << error.what() << '\n';
    return 1;
}
