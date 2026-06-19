// Contract test for the stable C ABI (include/adm/c_api.h).
//
// Exercises the ABI exactly as an external consumer would: version queries,
// context/result lifecycle, every error path, and one real end-to-end render of
// a runtime-generated ADM fixture (no private audio material; see CLAUDE.md).
// The default C ABI render (no options) resolves to binaural 2ch output.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/c_api.h"

// libadm / libbw64 — used here only to construct the fixture, never via the ABI.
#include <adm/adm.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

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
std::filesystem::path write_fixture(uint32_t sample_rate = 48000U) {
    auto doc = adm::Document::create();

    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TestCF"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{30.0F}, adm::Elevation{10.0F}}};
        block.set(adm::Gain{0.8F});
        cf->add(block);
    }
    doc->add(cf);

    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TestPF"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);

    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TestSF"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);

    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TestTF"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);

    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);

    auto object = adm::AudioObject::create(adm::AudioObjectName{"TestObject"});
    object->addReference(uid);
    doc->add(object);

    auto content = adm::AudioContent::create(adm::AudioContentName{"TestContent"});
    content->addReference(object);
    doc->add(content);

    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"TestProgramme"});
    programme->addReference(content);
    doc->add(programme);

    adm::reassignIds(doc);
    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());

    std::ostringstream buf;
    adm::writeXml(buf, doc);

    auto path = unique_temp_wav_path("mr_c_api_fixture");
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, static_cast<uint16_t>(sample_rate), 24U, chna, axml);
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

struct FullProgressState {
    struct Event {
        double fraction;
        std::string stage;
        bool message_non_null{false};
    };
    std::vector<Event> events;
};

void full_progress_cb(double fraction, const char* stage, const char* message, void* user_data) {
    auto* state = static_cast<FullProgressState*>(user_data);
    state->events.push_back({fraction, stage != nullptr ? stage : "", message != nullptr});
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

bool verify_version() {
    return check(adm_api_version_major() == ADM_API_VERSION_MAJOR, "version major mismatch") &&
           check(adm_api_version_minor() == ADM_API_VERSION_MINOR, "version minor mismatch") &&
           check(adm_api_version_patch() == ADM_API_VERSION_PATCH, "version patch mismatch");
}

bool verify_null_result_accessors() {
    return check(adm_render_result_error_code(nullptr) == ADM_ERROR_INVALID_ARGUMENT,
                 "error_code(nullptr) should be INVALID_ARGUMENT") &&
           check(adm_render_result_message(nullptr) != nullptr, "message(nullptr) should be non-null");
}

bool verify_error_paths(adm_context_t* ctx) {
    if (!check(adm_render_file(nullptr, "x.wav", nullptr, nullptr, nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "null context should be INVALID_ARGUMENT")) {
        return false;
    }
    if (!check(adm_render_file(ctx, nullptr, nullptr, nullptr, nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "null input should be INVALID_ARGUMENT")) {
        return false;
    }
    if (!check(adm_render_file(ctx, "", nullptr, nullptr, nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "empty input should be INVALID_ARGUMENT")) {
        return false;
    }

    // Nonexistent input: non-OK, and the result handle must carry the same code + a message.
    adm_render_result_t* result = nullptr;
    const auto missing = unique_temp_wav_path("mr_c_api_does_not_exist");
    const adm_error_code_t code = adm_render_file(ctx, missing.string().c_str(), nullptr, nullptr, nullptr, &result);
    const bool ok = check(code != ADM_ERROR_OK, "missing input should fail") &&
                    check(result != nullptr, "missing input should still allocate a result") &&
                    check(adm_render_result_error_code(result) == code, "result code should match return code") &&
                    check(!std::string{adm_render_result_message(result)}.empty(), "result message should be set");
    adm_destroy_render_result(result);
    return ok;
}

bool verify_successful_render(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto output = unique_temp_wav_path("mr_c_api_out");
    std::error_code ec;
    std::filesystem::remove(output, ec);
    FileGuard out_guard(output);

    ProgressState state;
    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file(ctx, input.string().c_str(), output.string().c_str(), &progress_cb, &state, &result);

    bool ok = check(code == ADM_ERROR_OK, "fixture render should succeed");
    ok = check(result != nullptr, "successful render should allocate a result") && ok;
    if (result != nullptr) {
        ok = check(adm_render_result_error_code(result) == ADM_ERROR_OK, "result code should be OK") && ok;
        // Message pointer must stay valid/stable until destroy.
        const char* msg1 = adm_render_result_message(result);
        const char* msg2 = adm_render_result_message(result);
        ok = check(msg1 != nullptr && msg1 == msg2, "result message pointer should be stable") && ok;
    }
    ok = check(std::filesystem::exists(output) && std::filesystem::file_size(output) > 0U,
               "render should produce a non-empty output file") &&
         ok;
    ok = check(state.calls > 0, "progress callback should fire") && ok;
    ok = check(state.last_stage == "finished", "final progress stage should be 'finished'") && ok;
    ok = check(state.last_fraction >= 0.99, "final progress fraction should reach ~1.0") && ok;

    adm_destroy_render_result(result);
    // result == NULL must suppress allocation but still return the code.
    const adm_error_code_t code2 =
        adm_render_file(ctx, input.string().c_str(), output.string().c_str(), nullptr, nullptr, nullptr);
    ok = check(code2 == ADM_ERROR_OK, "render with result==NULL should still succeed") && ok;
    return ok;
}

// ── v1.1 tests ────────────────────────────────────────────────────────────

bool verify_version_11() {
    return check(adm_api_version_minor() >= 1, "v1.1: minor version should be >= 1");
}

bool verify_options_null_setters() {
    bool ok = check(adm_render_options_set_renderer(nullptr, ADM_RENDERER_BINAURAL) == ADM_ERROR_OK,
                    "NULL opts set_renderer should return OK");
    ok = check(adm_render_options_set_output_layout(nullptr, "5.1") == ADM_ERROR_OK,
               "NULL opts set_output_layout should return OK") &&
         ok;
    ok = check(adm_render_options_set_output_bit_depth(nullptr, ADM_BIT_DEPTH_I24) == ADM_ERROR_OK,
               "NULL opts set_output_bit_depth should return OK") &&
         ok;
    ok = check(adm_render_options_set_speaker_spread_mode(nullptr, ADM_SPEAKER_SPREAD_MDAP) == ADM_ERROR_OK,
               "NULL opts set_speaker_spread_mode should return OK") &&
         ok;
    ok = check(adm_render_options_set_binaural_spread_mode(nullptr, ADM_BINAURAL_SPREAD_CLOUD) == ADM_ERROR_OK,
               "NULL opts set_binaural_spread_mode should return OK") &&
         ok;
    ok = check(adm_render_options_set_apac_container(nullptr, ADM_APAC_CONTAINER_CAF) == ADM_ERROR_OK,
               "NULL opts set_apac_container should return OK") &&
         ok;
    ok = check(adm_render_options_set_loudness_target(nullptr, -23.0) == ADM_ERROR_OK,
               "NULL opts set_loudness_target should return OK") &&
         ok;
    // void bool setters: null opts must not crash
    adm_render_options_set_peak_limit(nullptr, 1);
    adm_render_options_set_peak_normalize_to_limit(nullptr, 1);
    adm_render_options_set_apac_drc_music(nullptr, 1);
    // string setters: null opts → ADM_ERROR_OK (no-op)
    ok = check(adm_render_options_set_sofa_path(nullptr, "/any/path.sofa") == ADM_ERROR_OK,
               "NULL opts set_sofa_path should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_policy_path(nullptr, "/any/policy.json") == ADM_ERROR_OK,
               "NULL opts set_semantic_policy_path should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_report_path(nullptr, "/any/report.json") == ADM_ERROR_OK,
               "NULL opts set_semantic_report_path should return OK") &&
         ok;
    ok = check(adm_render_options_set_iamf_layers(nullptr, "5.1,7.1.4") == ADM_ERROR_OK,
               "NULL opts set_iamf_layers should return OK") &&
         ok;
    return ok;
}

bool verify_options_invalid_values(adm_render_options_t* opts) {
    bool ok = true;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_renderer(opts, static_cast<adm_renderer_t>(99)) == ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range renderer should return INVALID_ARGUMENT") &&
         ok;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_output_bit_depth(opts, static_cast<adm_output_bit_depth_t>(99)) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range bit_depth should return INVALID_ARGUMENT") &&
         ok;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_speaker_spread_mode(opts, static_cast<adm_speaker_spread_mode_t>(99)) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range speaker_spread should return INVALID_ARGUMENT") &&
         ok;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_binaural_spread_mode(opts, static_cast<adm_binaural_spread_mode_t>(99)) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range binaural_spread should return INVALID_ARGUMENT") &&
         ok;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_iamf_container(opts, static_cast<adm_iamf_container_t>(99)) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range iamf_container should return INVALID_ARGUMENT") &&
         ok;
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    ok = check(adm_render_options_set_apac_container(opts, static_cast<adm_apac_container_t>(99)) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "out-of-range apac_container should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_output_layout(opts, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "NULL layout should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_loudness_target(opts, std::numeric_limits<double>::quiet_NaN()) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "NaN loudness_target should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_loudness_target(opts, std::numeric_limits<double>::infinity()) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "inf loudness_target should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_peak_limit_dbtp(opts, std::numeric_limits<double>::quiet_NaN()) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "NaN peak_limit_dbtp should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_loudness_target(opts, 0.1) == ADM_ERROR_INVALID_ARGUMENT,
               "loudness_target > 0 should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_loudness_target(opts, -70.1) == ADM_ERROR_INVALID_ARGUMENT,
               "loudness_target < -70 should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_peak_limit_dbtp(opts, 0.1) == ADM_ERROR_INVALID_ARGUMENT,
               "peak_limit_dbtp > 0 should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_peak_limit_dbtp(opts, -60.1) == ADM_ERROR_INVALID_ARGUMENT,
               "peak_limit_dbtp < -60 should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_opus_bitrate_per_ch_kbps(opts, 5) == ADM_ERROR_INVALID_ARGUMENT,
               "opus_bitrate 5 (< 6) should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_opus_bitrate_per_ch_kbps(opts, 321) == ADM_ERROR_INVALID_ARGUMENT,
               "opus_bitrate 321 (> 320) should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 63) == ADM_ERROR_INVALID_ARGUMENT,
               "apac_bitrate 63 (< 64) should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 32769) == ADM_ERROR_INVALID_ARGUMENT,
               "apac_bitrate 32769 (> 32768) should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_default_interp_ms(opts, 501) == ADM_ERROR_INVALID_ARGUMENT,
               "interp_ms 501 (> 500) should return INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_options_set_object_smoothing_frames(opts, 48001) == ADM_ERROR_INVALID_ARGUMENT,
               "smoothing_frames 48001 (> 48000) should return INVALID_ARGUMENT") &&
         ok;
    return ok;
}

bool verify_options_boundary_values(adm_render_options_t* opts) {
    bool ok = check(adm_render_options_set_loudness_target(opts, -70.0) == ADM_ERROR_OK,
                    "loudness_target -70.0 (boundary) should return OK");
    ok = check(adm_render_options_set_loudness_target(opts, 0.0) == ADM_ERROR_OK,
               "loudness_target 0.0 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_peak_limit_dbtp(opts, -60.0) == ADM_ERROR_OK,
               "peak_limit_dbtp -60.0 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_peak_limit_dbtp(opts, 0.0) == ADM_ERROR_OK,
               "peak_limit_dbtp 0.0 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_opus_bitrate_per_ch_kbps(opts, 0) == ADM_ERROR_OK,
               "opus_bitrate 0 (auto) should return OK") &&
         ok;
    ok = check(adm_render_options_set_opus_bitrate_per_ch_kbps(opts, 128) == ADM_ERROR_OK,
               "opus_bitrate 128 should return OK") &&
         ok;
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 0) == ADM_ERROR_OK,
               "apac_bitrate 0 (auto) should return OK") &&
         ok;
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 32768) == ADM_ERROR_OK,
               "apac_bitrate 32768 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_apac_container(opts, ADM_APAC_CONTAINER_CAF) == ADM_ERROR_OK,
               "apac_container CAF should return OK") &&
         ok;
    ok = check(adm_render_options_set_apac_container(opts, ADM_APAC_CONTAINER_MPEG4) == ADM_ERROR_OK,
               "apac_container MPEG4 should return OK") &&
         ok;
    ok = check(adm_render_options_set_default_interp_ms(opts, 500) == ADM_ERROR_OK,
               "interp_ms 500 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_object_smoothing_frames(opts, 48000) == ADM_ERROR_OK,
               "smoothing_frames 48000 (boundary) should return OK") &&
         ok;
    ok = check(adm_render_options_set_renderer(opts, ADM_RENDERER_HOA) == ADM_ERROR_OK,
               "valid renderer should return OK") &&
         ok;
    ok = check(adm_render_options_set_renderer(opts, ADM_RENDERER_SAF_BINAURAL) == ADM_ERROR_OK,
               "valid saf-binaural renderer should return OK") &&
         ok;
    ok = check(adm_render_options_set_output_layout(opts, "7.1.4") == ADM_ERROR_OK, "valid layout should return OK") &&
         ok;
    ok = check(adm_render_options_set_iamf_layers(opts, "5.1,5.1.2,5.1.4,7.1.4") == ADM_ERROR_OK,
               "iamf_layers valid CSV should return OK") &&
         ok;
    ok = check(adm_render_options_set_iamf_layers(opts, nullptr) == ADM_ERROR_OK,
               "iamf_layers nullptr should clear and return OK") &&
         ok;
    ok = check(adm_render_options_set_iamf_layers(opts, "") == ADM_ERROR_OK,
               "iamf_layers empty string should clear and return OK") &&
         ok;
    // void bool setters: 0/1 accepted, no crash.
    adm_render_options_set_peak_limit(opts, 1);
    adm_render_options_set_peak_limit(opts, 0);
    adm_render_options_set_peak_normalize_to_limit(opts, 1);
    adm_render_options_set_peak_normalize_to_limit(opts, 0);
    adm_render_options_set_apac_drc_music(opts, 1);
    adm_render_options_set_apac_drc_music(opts, 0);
    // sofa_path: nullptr/"" → built-in KEMAR (clears field); valid path → stored.
    ok = check(adm_render_options_set_sofa_path(opts, nullptr) == ADM_ERROR_OK,
               "sofa_path nullptr (built-in KEMAR) should return OK") &&
         ok;
    ok = check(adm_render_options_set_sofa_path(opts, "") == ADM_ERROR_OK,
               "sofa_path \"\" (built-in KEMAR) should return OK") &&
         ok;
    ok = check(adm_render_options_set_sofa_path(opts, "/valid/path.sofa") == ADM_ERROR_OK,
               "sofa_path valid string should return OK") &&
         ok;
    // semantic paths: nullptr/"" → clears the field; valid path → stored.
    ok = check(adm_render_options_set_semantic_policy_path(opts, nullptr) == ADM_ERROR_OK,
               "semantic_policy_path nullptr (clear) should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_policy_path(opts, "") == ADM_ERROR_OK,
               "semantic_policy_path \"\" (clear) should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_policy_path(opts, "/valid/policy.json") == ADM_ERROR_OK,
               "semantic_policy_path valid string should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_report_path(opts, nullptr) == ADM_ERROR_OK,
               "semantic_report_path nullptr (clear) should return OK") &&
         ok;
    ok = check(adm_render_options_set_semantic_report_path(opts, "/valid/report.json") == ADM_ERROR_OK,
               "semantic_report_path valid string should return OK") &&
         ok;
    return ok;
}

bool verify_options_lifecycle() {
    bool ok = verify_options_null_setters();

    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts != nullptr) {
        ok = verify_options_invalid_values(opts) && ok;
        ok = verify_options_boundary_values(opts) && ok;
    }
    adm_destroy_render_options(opts);
    adm_destroy_render_options(nullptr); // must not crash
    return ok;
}

// ── v1.2 tests ────────────────────────────────────────────────────────────

bool verify_version_12() {
    return check(adm_api_version_minor() >= 2, "v1.2: minor version should be >= 2");
}

// Output time-range trim setters: NULL opts are no-ops; start rejects negative /
// non-finite; end accepts a positive time, treats sec <= 0 as "clear", and rejects
// non-finite.
bool verify_render_trim_setters() {
    bool ok = check(adm_render_options_set_render_start_sec(nullptr, 1.0) == ADM_ERROR_OK,
                    "NULL opts set_render_start_sec should return OK");
    ok = check(adm_render_options_set_render_end_sec(nullptr, 2.0) == ADM_ERROR_OK,
               "NULL opts set_render_end_sec should return OK") &&
         ok;

    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts != nullptr) {
        ok = check(adm_render_options_set_render_start_sec(opts, 0.0) == ADM_ERROR_OK, "start 0.0 should return OK") &&
             ok;
        ok = check(adm_render_options_set_render_start_sec(opts, 1.5) == ADM_ERROR_OK, "start 1.5 should return OK") &&
             ok;
        ok = check(adm_render_options_set_render_end_sec(opts, 4.0) == ADM_ERROR_OK, "end 4.0 should return OK") && ok;
        ok = check(adm_render_options_set_render_end_sec(opts, 0.0) == ADM_ERROR_OK,
                   "end 0.0 (clear) should return OK") &&
             ok;
        ok = check(adm_render_options_set_render_end_sec(opts, -1.0) == ADM_ERROR_OK,
                   "end -1.0 (clear) should return OK") &&
             ok;
        ok = check(adm_render_options_set_render_start_sec(opts, -0.1) == ADM_ERROR_INVALID_ARGUMENT,
                   "negative start should return INVALID_ARGUMENT") &&
             ok;
        ok = check(adm_render_options_set_render_start_sec(opts, std::numeric_limits<double>::quiet_NaN()) ==
                       ADM_ERROR_INVALID_ARGUMENT,
                   "NaN start should return INVALID_ARGUMENT") &&
             ok;
        ok = check(adm_render_options_set_render_end_sec(opts, std::numeric_limits<double>::infinity()) ==
                       ADM_ERROR_INVALID_ARGUMENT,
                   "inf end should return INVALID_ARGUMENT") &&
             ok;
    }
    adm_destroy_render_options(opts);
    return ok;
}

bool verify_render_file_ex_compat(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out_old = unique_temp_wav_path("mr_c_api_compat_old");
    const auto out_new = unique_temp_wav_path("mr_c_api_compat_new");
    FileGuard g1(out_old);
    FileGuard g2(out_new);

    adm_render_result_t* r1 = nullptr;
    adm_render_result_t* r2 = nullptr;
    const adm_error_code_t c1 =
        adm_render_file(ctx, input.string().c_str(), out_old.string().c_str(), nullptr, nullptr, &r1);
    const adm_error_code_t c2 =
        adm_render_file_ex(ctx, input.string().c_str(), out_new.string().c_str(), nullptr, nullptr, nullptr, &r2);

    bool ok = check(c1 == ADM_ERROR_OK && c2 == ADM_ERROR_OK,
                    "v1.0 adm_render_file and v1.1 adm_render_file_ex(NULL opts) should both succeed") &&
              check(adm_render_result_error_code(r1) == ADM_ERROR_OK, "r1 code should be OK") &&
              check(adm_render_result_error_code(r2) == ADM_ERROR_OK, "r2 code should be OK");

    const auto sz1 = std::filesystem::file_size(out_old);
    const auto sz2 = std::filesystem::file_size(out_new);
    ok = check(sz1 > 0U && sz1 == sz2, "render_file and render_file_ex(NULL) should produce same-size output") && ok;

    adm_destroy_render_result(r1);
    adm_destroy_render_result(r2);
    return ok;
}

// Read the channel count from a WAV header (bytes 22-23, uint16 LE) without
// using bw64, which may reject WAVE_FORMAT_EXTENSIBLE (multi-channel output).
[[nodiscard]] uint32_t wav_channel_count(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0U;
    }
    std::array<char, 24> buf{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!f.read(buf.data(), static_cast<std::streamsize>(buf.size()))) {
        return 0U;
    }
    // WAV: RIFF[4] size[4] WAVE[4] "fmt "[4] chunk_size[4] fmt_type[2] channels[2]
    if (std::string_view{buf.data(), 4} != "RIFF" || std::string_view{buf.data() + 8, 4} != "WAVE") {
        return 0U;
    }
    return static_cast<uint32_t>(static_cast<uint8_t>(buf[22])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(buf[23])) << 8U);
}

// Count sample frames in a WAV by walking its chunks (channels + bits from fmt,
// payload size from data). Avoids bw64, which may reject multi-channel EXTENSIBLE.
[[nodiscard]] uint64_t wav_frame_count(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0U;
    }
    const auto rd_u16 = [](const unsigned char* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U);
    };
    const auto rd_u32 = [](const unsigned char* p) {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
               (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
    };
    std::array<unsigned char, 12> riff{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!f.read(reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size()))) {
        return 0U;
    }
    if (std::string_view{reinterpret_cast<const char*>(riff.data()), 4} != "RIFF" ||
        std::string_view{reinterpret_cast<const char*>(riff.data()) + 8, 4} != "WAVE") {
        return 0U;
    }
    uint32_t channels = 0U;
    uint32_t bits = 0U;
    uint32_t data_size = 0U;
    std::array<unsigned char, 8> hdr{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    while (f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()))) {
        const std::string_view id{reinterpret_cast<const char*>(hdr.data()), 4};
        const uint32_t sz = rd_u32(hdr.data() + 4);
        if (id == "fmt ") {
            std::vector<unsigned char> fmt(sz);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            if (!f.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(sz))) {
                return 0U;
            }
            channels = rd_u16(fmt.data() + 2); // nChannels
            bits = rd_u16(fmt.data() + 14);    // wBitsPerSample
            f.seekg(sz & 1U, std::ios::cur);   // skip RIFF pad byte
        } else if (id == "data") {
            data_size = sz;
            break;
        } else {
            f.seekg(static_cast<std::streamoff>(sz) + (sz & 1U), std::ios::cur);
        }
    }
    if (channels == 0U || bits == 0U) {
        return 0U;
    }
    return static_cast<uint64_t>(data_size) / (static_cast<uint64_t>(channels) * (bits / 8U));
}

// Larger fixture for loudness tests: 1 second of signal to satisfy EBU R128
// integrated loudness gating (requires at least one complete 400ms window).
std::filesystem::path write_fixture_1s() {
    auto doc = adm::Document::create();
    auto cf = adm::AudioChannelFormat::create(adm::AudioChannelFormatName{"TestCF2"}, adm::TypeDefinition::OBJECTS);
    {
        adm::AudioBlockFormatObjects block{adm::SphericalPosition{adm::Azimuth{0.0F}, adm::Elevation{0.0F}}};
        block.set(adm::Gain{1.0F});
        cf->add(block);
    }
    doc->add(cf);
    auto pf = adm::AudioPackFormat::create(adm::AudioPackFormatName{"TestPF2"}, adm::TypeDefinition::OBJECTS);
    pf->addReference(cf);
    doc->add(pf);
    auto sf = adm::AudioStreamFormat::create(adm::AudioStreamFormatName{"TestSF2"}, adm::FormatDefinition::PCM);
    sf->setReference(cf);
    doc->add(sf);
    auto tf = adm::AudioTrackFormat::create(adm::AudioTrackFormatName{"TestTF2"}, adm::FormatDefinition::PCM);
    tf->setReference(sf);
    sf->addReference(tf);
    doc->add(tf);
    auto uid = adm::AudioTrackUid::create();
    uid->setReference(tf);
    uid->setReference(pf);
    doc->add(uid);
    auto object = adm::AudioObject::create(adm::AudioObjectName{"TestObject2"});
    object->addReference(uid);
    doc->add(object);
    auto content = adm::AudioContent::create(adm::AudioContentName{"TestContent2"});
    content->addReference(object);
    doc->add(content);
    auto programme = adm::AudioProgramme::create(adm::AudioProgrammeName{"TestProgramme2"});
    programme->addReference(content);
    doc->add(programme);
    adm::reassignIds(doc);
    const std::string uid_str = adm::formatId(uid->get<adm::AudioTrackUidId>());
    std::ostringstream buf;
    adm::writeXml(buf, doc);
    auto path = unique_temp_wav_path("mr_c_api_fixture_1s");
    auto chna = std::make_shared<bw64::ChnaChunk>(std::vector<bw64::AudioId>{bw64::AudioId(1, uid_str, "", "")});
    auto axml = std::make_shared<bw64::AxmlChunk>(buf.str());
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    constexpr uint64_t k_frames = 48000U; // 1 s @ 48 kHz
    std::vector<float> samples(k_frames, 0.5F);
    writer->write(samples.data(), k_frames);
    return path;
}

bool verify_hoa_render(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto output = unique_temp_wav_path("mr_c_api_hoa3");
    FileGuard guard(output);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_renderer(opts, ADM_RENDERER_HOA);
    adm_render_options_set_output_layout(opts, "hoa3");

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);

    bool ok = check(code == ADM_ERROR_OK, "HOA hoa3 render should succeed");
    ok = check(wav_channel_count(output) == 16U, "HOA hoa3 output should have 16 channels (3rd order ACN/SN3D)") && ok;

    const char* out_path = adm_render_result_output_path(result);
    ok = check(out_path != nullptr && std::string{out_path} == output.string(),
               "result output_path should match requested path") &&
         ok;

    adm_destroy_render_result(result);
    return ok;
}

bool verify_51_render(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto output = unique_temp_wav_path("mr_c_api_51");
    FileGuard guard(output);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_output_layout(opts, "5.1");

    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, nullptr);
    adm_destroy_render_options(opts);

    bool ok = check(code == ADM_ERROR_OK, "5.1 render should succeed");
    ok = check(wav_channel_count(output) == 6U, "5.1 output should have 6 channels") && ok;
    return ok;
}

bool verify_loudness_metrics(adm_context_t* ctx, const std::filesystem::path& input_1s) {
    const auto& input = input_1s;
    // First render without loudness normalisation to get the raw renderer LUFS (P2 baseline).
    double raw_lufs = 0.0;
    {
        const auto out_raw = unique_temp_wav_path("mr_c_api_raw");
        FileGuard g(out_raw);
        adm_render_result_t* r = nullptr;
        adm_render_file_ex(ctx, input.string().c_str(), out_raw.string().c_str(), nullptr, nullptr, nullptr, &r);
        adm_render_result_loudness_lufs(r, &raw_lufs);
        adm_destroy_render_result(r);
    }
    const auto output = unique_temp_wav_path("mr_c_api_lufs");
    FileGuard guard(output);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_loudness_target(opts, -23.0);

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);

    bool ok = check(code == ADM_ERROR_OK, "loudness normalised render should succeed");

    double lufs = 0.0;
    double peak = 0.0;
    ok = check(adm_render_result_loudness_lufs(result, &lufs) == 1, "loudness_lufs should be present") && ok;
    // P2 fix: result now returns post-gain (adjusted) metrics. Verify that the
    // adjusted LUFS is strictly closer to the -23 LUFS target than the raw
    // renderer value (captured above without loudness normalisation). Peak limiting
    // can prevent exact target attainment, so we only assert the direction.
    ok = check(std::abs(lufs - (-23.0)) < std::abs(raw_lufs - (-23.0)),
               "loudness_lufs after normalisation should be closer to -23 LUFS target than raw value") &&
         ok;
    ok = check(adm_render_result_peak_dbtp(result, &peak) == 1, "peak_dbtp should be present") && ok;
    ok = check(peak <= 0.0 && peak > -40.0, "peak_dbtp should be a plausible negative dBTP value") && ok;
    ok = check(adm_render_result_loudness_lufs(result, nullptr) == 1, "loudness_lufs(out=NULL) should return 1") && ok;

    adm_destroy_render_result(result);
    return ok;
}

// v1.2 end-to-end: --start/--end wire through adm_render_file_ex and actually
// shorten the output. input_1s is 1 s @ 48 kHz; [0.25s, 0.75s) keeps 24000 frames.
bool verify_render_trim_wire_through(adm_context_t* ctx, const std::filesystem::path& input_1s) {
    const auto out_full = unique_temp_wav_path("mr_c_api_trim_full");
    const auto out_trim = unique_temp_wav_path("mr_c_api_trim_win");
    FileGuard g_full(out_full);
    FileGuard g_trim(out_trim);

    // Baseline: no trim → full 1 s (48000 frames).
    adm_render_result_t* r_full = nullptr;
    const adm_error_code_t code_full = adm_render_file_ex(
        ctx, input_1s.string().c_str(), out_full.string().c_str(), nullptr, nullptr, nullptr, &r_full);
    bool ok = check(code_full == ADM_ERROR_OK, "untrimmed render should succeed");
    adm_destroy_render_result(r_full);
    ok = check(wav_frame_count(out_full) == 48000U, "untrimmed output should be 48000 frames (1 s)") && ok;

    // Trimmed: [0.25s, 0.75s) → 24000 frames.
    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "trim options creation should succeed") && ok;
    if (opts == nullptr) {
        return false;
    }
    ok = check(adm_render_options_set_render_start_sec(opts, 0.25) == ADM_ERROR_OK, "set_render_start_sec(0.25)") && ok;
    ok = check(adm_render_options_set_render_end_sec(opts, 0.75) == ADM_ERROR_OK, "set_render_end_sec(0.75)") && ok;

    adm_render_result_t* r_trim = nullptr;
    const adm_error_code_t code_trim =
        adm_render_file_ex(ctx, input_1s.string().c_str(), out_trim.string().c_str(), opts, nullptr, nullptr, &r_trim);
    ok = check(code_trim == ADM_ERROR_OK, "trimmed render should succeed") && ok;
    ok = check(r_trim != nullptr && adm_render_result_error_code(r_trim) == ADM_ERROR_OK, "trim result OK") && ok;
    adm_destroy_render_result(r_trim);
    adm_destroy_render_options(opts);

    const uint64_t frames = wav_frame_count(out_trim);
    ok = check(frames == 24000U, "trimmed output should be 24000 frames (0.5 s window)") && ok;
    return ok;
}

// ── v1.3 tests ────────────────────────────────────────────────────────────

bool verify_version_13() {
    return check(adm_api_version_minor() >= 3, "v1.3: minor version should be >= 3");
}

// Final-gain setter: NULL opts are a no-op; any finite value is accepted (no range
// limit, by design); non-finite is rejected.
bool verify_final_gain_setter() {
    bool ok = check(adm_render_options_set_final_gain_db(nullptr, 6.0) == ADM_ERROR_OK,
                    "NULL opts set_final_gain_db should return OK");

    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts != nullptr) {
        ok = check(adm_render_options_set_final_gain_db(opts, 0.0) == ADM_ERROR_OK, "final gain 0.0 OK") && ok;
        ok = check(adm_render_options_set_final_gain_db(opts, 12.0) == ADM_ERROR_OK, "final gain +12 OK") && ok;
        ok = check(adm_render_options_set_final_gain_db(opts, -30.0) == ADM_ERROR_OK, "final gain -30 OK") && ok;
        ok = check(adm_render_options_set_final_gain_db(opts, 100.0) == ADM_ERROR_OK,
                   "final gain +100 (no range limit) OK") &&
             ok;
        ok = check(adm_render_options_set_final_gain_db(opts, std::numeric_limits<double>::quiet_NaN()) ==
                       ADM_ERROR_INVALID_ARGUMENT,
                   "NaN final gain should return INVALID_ARGUMENT") &&
             ok;
        ok = check(adm_render_options_set_final_gain_db(opts, std::numeric_limits<double>::infinity()) ==
                       ADM_ERROR_INVALID_ARGUMENT,
                   "inf final gain should return INVALID_ARGUMENT") &&
             ok;
    }
    adm_destroy_render_options(opts);
    return ok;
}

// v1.3 end-to-end: +20 dB final gain wires through adm_render_file_ex, bypasses
// the peak ceiling (default -1 dBTP), and is reflected in the reported peak.
bool verify_final_gain_wire_through(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out_a = unique_temp_wav_path("mr_c_api_fg_base");
    const auto out_b = unique_temp_wav_path("mr_c_api_fg_boost");
    FileGuard g_a(out_a);
    FileGuard g_b(out_b);

    // Baseline render (defaults: peak limit on, no final gain).
    adm_render_result_t* r_a = nullptr;
    adm_render_file_ex(ctx, input.string().c_str(), out_a.string().c_str(), nullptr, nullptr, nullptr, &r_a);
    double peak_a = 0.0;
    bool ok = check(adm_render_result_peak_dbtp(r_a, &peak_a) == 1, "baseline peak_dbtp should be present");
    adm_destroy_render_result(r_a);

    // +20 dB final gain.
    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts == nullptr) {
        return false;
    }
    ok = check(adm_render_options_set_final_gain_db(opts, 20.0) == ADM_ERROR_OK, "set_final_gain_db(20)") && ok;
    adm_render_result_t* r_b = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), out_b.string().c_str(), opts, nullptr, nullptr, &r_b);
    ok = check(code == ADM_ERROR_OK, "final-gain render should succeed") && ok;
    double peak_b = 0.0;
    ok = check(adm_render_result_peak_dbtp(r_b, &peak_b) == 1, "final-gain peak_dbtp should be present") && ok;
    adm_destroy_render_result(r_b);
    adm_destroy_render_options(opts);

    // +20 dB bypasses the -1 dBTP ceiling: reported peak must exceed 0 dBTP (only
    // possible if final gain is unconstrained) and rise by ~20 dB (metrics reflect it).
    ok = check(peak_b > 0.0, "final gain should push reported peak above 0 dBTP (bypasses peak limit)") && ok;
    ok = check(std::abs((peak_b - peak_a) - 20.0) < 0.5, "reported peak should rise by ~20 dB (metrics reflect)") && ok;
    return ok;
}

// ── v1.4 tests ────────────────────────────────────────────────────────────

bool verify_version_14() {
    return check(adm_api_version_minor() >= 4, "v1.4: minor version should be >= 4");
}

// Cancel-token lifecycle + NULL safety: every entry point tolerates NULL, and
// create/cancel/reset/destroy and set_cancel_token never crash.
bool verify_cancel_token_lifecycle() {
    adm_cancel(nullptr);
    adm_reset_cancel_token(nullptr);
    adm_destroy_cancel_token(nullptr);
    adm_render_options_set_cancel_token(nullptr, nullptr);

    adm_cancel_token_t* token = adm_create_cancel_token();
    bool ok = check(token != nullptr, "cancel token creation should succeed");
    if (token == nullptr) {
        return false;
    }
    adm_cancel(token); // request
    adm_cancel(token); // idempotent
    adm_reset_cancel_token(token);

    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts != nullptr) {
        adm_render_options_set_cancel_token(opts, token);   // associate
        adm_render_options_set_cancel_token(opts, nullptr); // clear
    }
    adm_destroy_render_options(opts);
    adm_destroy_cancel_token(token);
    return ok;
}

// Deterministic end-to-end: cancelling before the render starts makes the engine
// abort at its first checkpoint, return ADM_ERROR_CANCELLED, and leave no output
// file. Resetting the same token then drives a successful render (token reuse).
bool verify_cancel_pre_render(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out = unique_temp_wav_path("mr_c_api_cancel");
    FileGuard g(out);

    adm_cancel_token_t* token = adm_create_cancel_token();
    bool ok = check(token != nullptr, "cancel token creation should succeed");
    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (token == nullptr || opts == nullptr) {
        adm_destroy_render_options(opts);
        adm_destroy_cancel_token(token);
        return false;
    }
    adm_render_options_set_cancel_token(opts, token);

    // Pre-cancel → render must report CANCELLED and write nothing.
    adm_cancel(token);
    adm_render_result_t* r = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r);
    ok = check(code == ADM_ERROR_CANCELLED, "pre-cancelled render should return CANCELLED") && ok;
    ok = check(r != nullptr && adm_render_result_error_code(r) == ADM_ERROR_CANCELLED, "result code CANCELLED") && ok;
    adm_destroy_render_result(r);
    ok = check(!std::filesystem::exists(out), "cancelled render should leave no output file") && ok;
    ok = check(!has_output_sidecar(out, "render_tmp"), "pre-cancelled render should leave no render_tmp sidecar") && ok;
    ok = check(!has_output_sidecar(out, "output_tmp"), "pre-cancelled render should leave no output_tmp sidecar") && ok;

    // Reset → same token + options now render successfully (token reuse).
    adm_reset_cancel_token(token);
    adm_render_result_t* r2 = nullptr;
    const adm_error_code_t code2 =
        adm_render_file_ex(ctx, input.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r2);
    ok = check(code2 == ADM_ERROR_OK, "render after reset should succeed") && ok;
    adm_destroy_render_result(r2);
    ok = check(std::filesystem::exists(out), "render after reset should produce output") && ok;

    adm_destroy_render_options(opts);
    adm_destroy_cancel_token(token);
    return ok;
}

// Concurrent cancel from another thread while a render runs. The render either
// finishes (OK) or is cancelled mid-flight; both are acceptable. The point is
// that cross-thread adm_cancel neither crashes nor deadlocks, and a cancelled
// render leaves no output behind. (Cross-thread safety rests on std::stop_source;
// only the worker thread touches ctx, so the context contract is respected.)
bool verify_cancel_threaded(adm_context_t* ctx, const std::filesystem::path& input_1s) {
    const auto out = unique_temp_wav_path("mr_c_api_cancel_thr");
    FileGuard g(out);

    adm_cancel_token_t* token = adm_create_cancel_token();
    adm_render_options_t* opts = adm_create_render_options();
    if (token == nullptr || opts == nullptr) {
        adm_destroy_render_options(opts);
        adm_destroy_cancel_token(token);
        return check(false, "threaded cancel: setup allocation failed");
    }
    adm_render_options_set_cancel_token(opts, token);

    adm_render_result_t* r = nullptr;
    adm_error_code_t code = ADM_ERROR_INTERNAL;
    std::thread worker([&] {
        code = adm_render_file_ex(ctx, input_1s.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r);
    });
    adm_cancel(token); // race the in-flight render
    worker.join();

    bool ok = check(code == ADM_ERROR_OK || code == ADM_ERROR_CANCELLED, "threaded render should end OK or CANCELLED");
    if (code == ADM_ERROR_CANCELLED) {
        ok = check(!std::filesystem::exists(out), "cancelled threaded render should leave no output") && ok;
        ok = check(!has_output_sidecar(out, "render_tmp"),
                   "cancelled threaded render should leave no render_tmp sidecar") &&
             ok;
        ok = check(!has_output_sidecar(out, "output_tmp"),
                   "cancelled threaded render should leave no output_tmp sidecar") &&
             ok;
    }
    adm_destroy_render_result(r);
    adm_destroy_render_options(opts);
    adm_destroy_cancel_token(token);
    return ok;
}

// ── v1.5 tests ────────────────────────────────────────────────────────────

bool verify_version_15() {
    return check(adm_api_version_minor() >= 5, "v1.5: minor version should be >= 5");
}

// Setter NULL safety + clear semantics for the in-memory semantic-policy entry points.
bool verify_semantic_memory_setters() {
    bool ok = check(adm_render_options_set_semantic_policy_json(nullptr, "x") == ADM_ERROR_OK,
                    "NULL opts set_semantic_policy_json should return OK");
    adm_render_options_set_capture_semantic_report(nullptr, 1); // NULL no-op, must not crash

    adm_render_options_t* opts = adm_create_render_options();
    ok = check(opts != nullptr, "options creation should succeed") && ok;
    if (opts != nullptr) {
        ok = check(adm_render_options_set_semantic_policy_json(opts, "{}") == ADM_ERROR_OK,
                   "set_semantic_policy_json valid string should return OK") &&
             ok;
        ok = check(adm_render_options_set_semantic_policy_json(opts, "") == ADM_ERROR_OK,
                   "set_semantic_policy_json \"\" (clear) should return OK") &&
             ok;
        ok = check(adm_render_options_set_semantic_policy_json(opts, nullptr) == ADM_ERROR_OK,
                   "set_semantic_policy_json nullptr (clear) should return OK") &&
             ok;
        adm_render_options_set_capture_semantic_report(opts, 1);
        adm_render_options_set_capture_semantic_report(opts, 0);
    }
    adm_destroy_render_options(opts);
    return ok;
}

// End-to-end: an in-memory policy that globally mutes the scene wires through
// adm_render_file_ex (no temp file), silences the output (peak metric becomes
// absent), and — with capture enabled — yields an in-memory report reflecting the
// mute. The result string is owned by the result handle (not adm_free_string).
bool verify_semantic_policy_json_wire_through(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out = unique_temp_wav_path("mr_c_api_poljson");
    FileGuard g(out);

    adm_render_options_t* opts = adm_create_render_options();
    bool ok = check(opts != nullptr, "options creation should succeed");
    if (opts == nullptr) {
        return false;
    }
    // Global mute via the in-memory policy JSON.
    const char* policy = R"({"schema":"mradm.semantic-policy.v1","global":{"gain":{"mute":true}}})";
    ok = check(adm_render_options_set_semantic_policy_json(opts, policy) == ADM_ERROR_OK, "set in-memory policy") && ok;
    adm_render_options_set_capture_semantic_report(opts, 1);

    adm_render_result_t* r = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r);
    ok = check(code == ADM_ERROR_OK, "in-memory policy render should succeed") && ok;

    // Captured report present, correct schema, and reflects the mute — proving the
    // in-memory JSON was parsed and applied to the scene before rendering.
    const char* report = adm_render_result_semantic_report_json(r);
    ok = check(report != nullptr, "captured semantic report should be non-NULL") && ok;
    if (report != nullptr) {
        const std::string text(report);
        ok = check(text.find("mradm.semantic-report.v1") != std::string::npos, "report should carry report schema") &&
             ok;
        ok =
            check(text.find("\"effective_mute\": true") != std::string::npos, "report should reflect effective mute") &&
            ok;
    }
    adm_destroy_render_result(r);

    // Without capture, the report accessor returns NULL even though a render ran.
    adm_render_options_set_capture_semantic_report(opts, 0);
    adm_render_result_t* r2 = nullptr;
    adm_render_file_ex(ctx, input.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r2);
    ok = check(adm_render_result_semantic_report_json(r2) == nullptr, "uncaptured report should be NULL") && ok;
    adm_destroy_render_result(r2);

    adm_destroy_render_options(opts);
    return ok;
}

// Malformed in-memory policy JSON surfaces as a render error (not at set time).
bool verify_semantic_policy_json_invalid(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out = unique_temp_wav_path("mr_c_api_polbad");
    FileGuard g(out);

    adm_render_options_t* opts = adm_create_render_options();
    bool ok = check(opts != nullptr, "options creation should succeed");
    if (opts == nullptr) {
        return false;
    }
    // Setter accepts any string; the error appears at render time.
    ok = check(adm_render_options_set_semantic_policy_json(opts, "{ not valid json") == ADM_ERROR_OK,
               "setter accepts malformed JSON (validated at render)") &&
         ok;
    adm_render_result_t* r = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), out.string().c_str(), opts, nullptr, nullptr, &r);
    ok = check(code != ADM_ERROR_OK, "malformed in-memory policy should fail the render") && ok;
    adm_destroy_render_result(r);
    adm_destroy_render_options(opts);
    return ok;
}

// A failure AFTER the semantic report is captured must still surface the report,
// per the c_api.h contract ("available regardless of the render's error code").
// FLAC + a height layout (7.1.4) is rejected during format validation, which runs
// after the report is built — a deterministic, cross-platform late-stage failure.
bool verify_semantic_report_on_late_failure(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto flac_out = std::filesystem::path(unique_temp_wav_path("mr_c_api_latefail")).replace_extension(".flac");
    FileGuard g(flac_out);

    adm_render_options_t* opts = adm_create_render_options();
    bool ok = check(opts != nullptr, "options creation should succeed");
    if (opts == nullptr) {
        return false;
    }
    ok = check(adm_render_options_set_output_layout(opts, "7.1.4") == ADM_ERROR_OK, "set 7.1.4 layout") && ok;
    const char* policy = R"({"schema":"mradm.semantic-policy.v1","global":{"gain":{"mute":true}}})";
    ok = check(adm_render_options_set_semantic_policy_json(opts, policy) == ADM_ERROR_OK, "set in-memory policy") && ok;
    adm_render_options_set_capture_semantic_report(opts, 1);

    adm_render_result_t* r = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), flac_out.string().c_str(), opts, nullptr, nullptr, &r);
    ok = check(code != ADM_ERROR_OK, "FLAC + height layout should fail at format validation") && ok;
    // The report, captured before the failure, must still be readable.
    const char* report = adm_render_result_semantic_report_json(r);
    ok = check(report != nullptr, "report should be readable even when a later stage fails") && ok;
    if (report != nullptr) {
        ok = check(std::string(report).find("mradm.semantic-report.v1") != std::string::npos,
                   "report schema present on failure") &&
             ok;
    }
    adm_destroy_render_result(r);
    adm_destroy_render_options(opts);
    return ok;
}

// Render succeeds into a staging WAV, then final Opus encoding rejects the 44.1 kHz
// source. The failed render must not leave the final .mka, render_tmp WAV, or
// output_tmp sidecar behind.
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

// ── v1.7 tests ────────────────────────────────────────────────────────────

bool verify_version_17() {
    return check(adm_api_version_minor() >= 7, "v1.7: minor version should be >= 7");
}

// adm_render_stage_from_string maps every documented stage string to its enum, and
// returns ADM_STAGE_UNKNOWN for NULL / empty / unrecognized input.
bool verify_render_stage_from_string() {
    bool ok = check(adm_render_stage_from_string("validating") == ADM_STAGE_VALIDATING, "stage: validating");
    ok = check(adm_render_stage_from_string("probing") == ADM_STAGE_PROBING, "stage: probing") && ok;
    ok =
        check(adm_render_stage_from_string("importing_scene") == ADM_STAGE_IMPORTING_SCENE, "stage: importing_scene") &&
        ok;
    ok = check(adm_render_stage_from_string("planning") == ADM_STAGE_PLANNING, "stage: planning") && ok;
    ok = check(adm_render_stage_from_string("rendering") == ADM_STAGE_RENDERING, "stage: rendering") && ok;
    ok =
        check(adm_render_stage_from_string("post_processing") == ADM_STAGE_POST_PROCESSING, "stage: post_processing") &&
        ok;
    ok = check(adm_render_stage_from_string("finished") == ADM_STAGE_FINISHED, "stage: finished") && ok;
    // Fallbacks.
    ok = check(adm_render_stage_from_string(nullptr) == ADM_STAGE_UNKNOWN, "stage: NULL → UNKNOWN") && ok;
    ok = check(adm_render_stage_from_string("") == ADM_STAGE_UNKNOWN, "stage: empty → UNKNOWN") && ok;
    ok = check(adm_render_stage_from_string("Rendering") == ADM_STAGE_UNKNOWN, "stage: case-sensitive → UNKNOWN") && ok;
    ok = check(adm_render_stage_from_string("nonsense") == ADM_STAGE_UNKNOWN, "stage: garbage → UNKNOWN") && ok;
    return ok;
}

// ── v1.8 tests ────────────────────────────────────────────────────────────

bool verify_version_18() {
    return check(adm_api_version_minor() >= 8, "v1.8: minor version should be >= 8");
}

// ── v1.9 tests ────────────────────────────────────────────────────────────

bool verify_version_19() {
    return check(adm_api_version_minor() >= 9, "v1.9: minor version should be >= 9");
}

// ── v1.10 tests ───────────────────────────────────────────────────────────

bool verify_version_110() {
    return check(adm_api_version_minor() >= 10, "v1.10: minor version should be >= 10");
}

// ── v1.11 tests ───────────────────────────────────────────────────────────

bool verify_version_111() {
    return check(adm_api_version_minor() >= 11, "v1.11: minor version should be >= 11");
}

// ── v1.12 tests ───────────────────────────────────────────────────────────

bool verify_version_112() {
    return check(adm_api_version_minor() >= 12, "v1.12: minor version should be >= 12");
}

// ── v1.14 tests ───────────────────────────────────────────────────────────

bool verify_version_114() {
    return check(adm_api_version_minor() >= 14, "v1.14: minor version should be >= 14");
}

bool verify_render_support_matrix_json(adm_context_t* ctx) {
    char* json = nullptr;
    const adm_error_code_t code = adm_render_support_matrix_json(ctx, &json);
    bool ok = check(code == ADM_ERROR_OK, "render_support_matrix_json: should succeed");
    ok = check(json != nullptr, "render_support_matrix_json: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        ok = check(has(R"("schema": "mradm.render-support-matrix")") && has(R"("schema_version": 1)"),
                   "render_support_matrix_json: should carry schema + schema_version") &&
             ok;
        ok = check(has("\"features\"") && has("\"backends\"") && has("\"layouts\"") && has("\"targets\"") &&
                       has("\"entries\""),
                   "render_support_matrix_json: should include features/backends/layouts/targets/entries") &&
             ok;
        ok = check(has(R"("target": "apac_mpeg4")") && has(R"("target": "apac_caf")") && has(R"("required_option")") &&
                       has(R"("value": "caf")"),
                   "render_support_matrix_json: should distinguish APAC MPEG-4 and CAF targets") &&
             ok;
        ok = check(has(R"("target": "iamf")") && has(R"("target": "iamf_mp4")"),
                   "render_support_matrix_json: should distinguish IAMF raw and MP4 targets") &&
             ok;
        ok = check(has(R"("renderer": "saf-binaural")") && has(R"("layout": "binaural")"),
                   "render_support_matrix_json: should include SAF binaural + binaural layout") &&
             ok;
        ok = check(has(R"("supported": false)") && has(R"("reason")"),
                   "render_support_matrix_json: should include unsupported rows with reasons") &&
             ok;
#ifdef __APPLE__
        ok = check(has(R"("apac": true)"), "render_support_matrix_json: apac feature true on macOS") && ok;
#else
        ok = check(has(R"("apac": false)") && has("macOS only (AudioToolbox)"),
                   "render_support_matrix_json: apac unavailable reason off-macOS") &&
             ok;
#endif
    }
    adm_free_string(json);

    char* bad = nullptr;
    ok = check(adm_render_support_matrix_json(nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "render_support_matrix_json: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_render_support_matrix_json(ctx, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "render_support_matrix_json: NULL out_json should be INVALID_ARGUMENT") &&
         ok;
    ok = check(bad == nullptr, "render_support_matrix_json: out_json should stay NULL on failure") && ok;
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

bool verify_progress_v2_render(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto out_null = unique_temp_wav_path("mr_c_api_prog_v2_null");
    FileGuard g0(out_null);
    bool ok = check(adm_render_file_ex2(
                        ctx, input.string().c_str(), out_null.string().c_str(), nullptr, nullptr, nullptr, nullptr) ==
                        ADM_ERROR_OK,
                    "v2 progress: NULL callback render should succeed");

    const auto output = unique_temp_wav_path("mr_c_api_prog_v2");
    FileGuard guard(output);
    ProgressV2State state;
    const adm_error_code_t code = adm_render_file_ex2(
        ctx, input.string().c_str(), output.string().c_str(), nullptr, &progress_v2_cb, &state, nullptr);
    ok = check(code == ADM_ERROR_OK, "v2 progress: render should succeed") && ok;
    ok = verify_progress_v2_events(state, true, "v2 progress render") && ok;
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

// Whether any of a result's captured log messages contains `needle`.
bool result_logs_contain(const adm_render_result_t* result, const char* needle) {
    const uint32_t n = adm_render_result_log_count(result);
    for (uint32_t i = 0; i < n; ++i) {
        const char* msg = nullptr;
        if (adm_render_result_log_entry(result, i, nullptr, nullptr, &msg) == 1 && msg != nullptr &&
            std::string(msg).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// PreviewSession: create once (import + policy), render multiple windows reusing the
// cached scene AND backend prepared state (HRTF / gain matrix), plus NULL/error safety.
// Default options resolve to binaural 2ch.
//
// Cache-reuse coverage rationale: the prepared-state cache lives in RenderService and is
// backend-agnostic (every backend's prepare() result flows through the same slot), so a
// single end-to-end proof that prepare() runs once is sufficient — done here via the
// binaural "HRTF source:" log (present on window 1, absent on window 2). The EAR-5.1
// session below additionally confirms a NON-binaural PreparedState round-trips through the
// ABI cache path. VBAP and HOA reuse the same cache path and their prepare/render_window
// split is bit-exact-verified in render_trim_fixture_test (window_bit_exact for ear/vbap/
// hoa/binaural), so they intentionally have no separate per-backend cache-reuse assertion
// here (there is no distinctive once-only prepare log to assert on for them).
// NOLINTNEXTLINE(readability-function-size)
bool verify_preview_session(adm_context_t* ctx, const std::filesystem::path& input_1s) {
    const std::string in = input_1s.string();

    // create() error paths.
    adm_preview_session_t* bad = nullptr;
    bool ok = check(adm_create_preview_session(nullptr, in.c_str(), nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
                    "preview create: NULL context → INVALID_ARGUMENT");
    ok = check(adm_create_preview_session(ctx, nullptr, nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "preview create: NULL input → INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_create_preview_session(ctx, in.c_str(), nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "preview create: NULL out → INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_create_preview_session(ctx, "/no/such/adm/file.wav", nullptr, &bad) != ADM_ERROR_OK,
               "preview create: missing file should fail") &&
         ok;
    ok = check(bad == nullptr, "preview create: out stays NULL on failure") && ok;

    // A real session.
    adm_preview_session_t* session = nullptr;
    ok = check(adm_create_preview_session(ctx, in.c_str(), nullptr, &session) == ADM_ERROR_OK,
               "preview create succeeds") &&
         ok;
    ok = check(session != nullptr, "preview session handle non-NULL") && ok;
    if (session == nullptr) {
        return false;
    }

    // render_window error paths.
    ok = check(adm_preview_render_window(nullptr, 0.0, 0.0, nullptr, nullptr, nullptr, nullptr) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "preview render: NULL session → INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_preview_render_window(session, -1.0, 0.0, nullptr, nullptr, nullptr, nullptr) ==
                   ADM_ERROR_INVALID_ARGUMENT,
               "preview render: negative start → INVALID_ARGUMENT") &&
         ok;

    // Multiple windows from the SAME session reuse the cached scene.
    const auto out_a = unique_temp_wav_path("mr_c_api_preview_a");
    const auto out_b = unique_temp_wav_path("mr_c_api_preview_b");
    const auto out_c = unique_temp_wav_path("mr_c_api_preview_c");
    const auto out_v2 = unique_temp_wav_path("mr_c_api_preview_v2");
    FileGuard ga(out_a);
    FileGuard gb(out_b);
    FileGuard gc(out_c);
    FileGuard gv2(out_v2);

    adm_render_result_t* ra = nullptr;
    ok = check(adm_preview_render_window(session, 0.25, 0.75, out_a.string().c_str(), nullptr, nullptr, &ra) ==
                   ADM_ERROR_OK,
               "preview window [0.25,0.75) succeeds") &&
         ok;
    // The first window builds the backend prepared state (binaural HRTF), logged once.
    ok = check(result_logs_contain(ra, "HRTF source:"), "first preview window builds HRTF (prepare ran)") && ok;
    adm_destroy_render_result(ra);
    ok = check(wav_frame_count(out_a) == 24000U, "preview window [0.25,0.75) → 24000 frames") && ok;

    adm_render_result_t* rb = nullptr;
    ok = check(adm_preview_render_window(session, 0.0, 0.5, out_b.string().c_str(), nullptr, nullptr, &rb) ==
                   ADM_ERROR_OK,
               "preview window [0,0.5) (reuses cached scene) succeeds") &&
         ok;
    // The second window reuses the cached prepared state: the HRTF is NOT rebuilt.
    ok =
        check(!result_logs_contain(rb, "HRTF source:"), "second preview window reuses cached HRTF (prepare skipped)") &&
        ok;
    adm_destroy_render_result(rb);
    ok = check(wav_frame_count(out_b) == 24000U, "preview window [0,0.5) → 24000 frames") && ok;

    // end_sec <= 0 means "to the end".
    adm_render_result_t* rc = nullptr;
    ok = check(adm_preview_render_window(session, 0.5, 0.0, out_c.string().c_str(), nullptr, nullptr, &rc) ==
                   ADM_ERROR_OK,
               "preview window [0.5, end) succeeds") &&
         ok;
    adm_destroy_render_result(rc);
    ok = check(wav_frame_count(out_c) == 24000U, "preview window [0.5, end) → 24000 frames (to end)") && ok;

    ProgressV2State preview_v2_state;
    adm_render_result_t* rv2 = nullptr;
    ok = check(adm_preview_render_window_v2(
                   session, 0.0, 0.25, out_v2.string().c_str(), &progress_v2_cb, &preview_v2_state, &rv2) ==
                   ADM_ERROR_OK,
               "preview v2 window succeeds") &&
         ok;
    adm_destroy_render_result(rv2);
    ok = verify_progress_v2_events(preview_v2_state, true, "preview v2 progress") && ok;
    ok = check(wav_frame_count(out_v2) == 12000U, "preview v2 window [0,0.25) → 12000 frames") && ok;

    adm_destroy_preview_session(session);
    adm_destroy_preview_session(nullptr); // must not crash

    // A non-binaural session (EAR 5.1) exercises the same scene + prepared-state cache
    // path through a backend with its own PreparedState (gain matrix), confirming the
    // generalized prepare/render_window split works end-to-end via the ABI.
    adm_render_options_t* ear_opts = adm_create_render_options();
    ok = check(ear_opts != nullptr, "ear preview options creation") && ok;
    if (ear_opts != nullptr) {
        adm_render_options_set_renderer(ear_opts, ADM_RENDERER_EAR);
        adm_render_options_set_output_layout(ear_opts, "5.1");
        adm_preview_session_t* ear_session = nullptr;
        ok = check(adm_create_preview_session(ctx, in.c_str(), ear_opts, &ear_session) == ADM_ERROR_OK,
                   "ear 5.1 preview session create") &&
             ok;
        if (ear_session != nullptr) {
            const auto out_e = unique_temp_wav_path("mr_c_api_preview_ear");
            FileGuard ge(out_e);
            adm_render_result_t* re = nullptr;
            ok = check(adm_preview_render_window(
                           ear_session, 0.25, 0.75, out_e.string().c_str(), nullptr, nullptr, &re) == ADM_ERROR_OK,
                       "ear 5.1 preview window succeeds") &&
                 ok;
            adm_destroy_render_result(re);
            ok = check(wav_frame_count(out_e) == 24000U, "ear 5.1 preview window → 24000 frames") && ok;
            ok = check(wav_channel_count(out_e) == 6U, "ear 5.1 preview window → 6 channels") && ok;
            adm_destroy_preview_session(ear_session);
        }
        adm_destroy_render_options(ear_opts);
    }
    return ok;
}

bool verify_probe(adm_context_t* ctx, const std::filesystem::path& input) {
    // Valid probe.
    adm_scene_info_t* info = nullptr;
    adm_error_code_t code = adm_probe_file(ctx, input.string().c_str(), &info);
    bool ok = check(code == ADM_ERROR_OK, "probe on fixture should succeed") &&
              check(info != nullptr, "probe should allocate scene info");
    if (info != nullptr) {
        ok = check(adm_scene_info_sample_rate(info) == 48000U, "probe: sample_rate should be 48000") && ok;
        ok = check(adm_scene_info_channels(info) == 1U, "probe: channels should be 1") && ok;
        ok = check(adm_scene_info_frames(info) == 9600U, "probe: frames should be 9600") && ok;
        ok = check(adm_scene_info_duration_seconds(info) > 0.19 && adm_scene_info_duration_seconds(info) < 0.21,
                   "probe: duration should be ~0.2s") &&
             ok;
        ok = check(adm_scene_info_programme_count(info) == 1U, "probe: programme_count should be 1") && ok;
        ok = check(adm_scene_info_object_count(info) == 1U, "probe: object_count should be 1") && ok;
    }
    adm_destroy_scene_info(info);
    adm_destroy_scene_info(nullptr); // must not crash

    // out==NULL: validate file, no allocation.
    code = adm_probe_file(ctx, input.string().c_str(), nullptr);
    ok = check(code == ADM_ERROR_OK, "probe with out==NULL should still succeed") && ok;

    // Null/empty input.
    ok = check(adm_probe_file(ctx, nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "probe: null input should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_probe_file(ctx, "", nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "probe: empty input should be INVALID_ARGUMENT") &&
         ok;

    // Nonexistent file.
    const auto missing = unique_temp_wav_path("mr_c_api_probe_missing");
    code = adm_probe_file(ctx, missing.string().c_str(), &info);
    ok = check(code != ADM_ERROR_OK, "probe: missing file should fail") && ok;
    ok = check(info == nullptr, "probe: missing file should leave info null") && ok;

    // Null accessors must return safe defaults.
    ok = check(adm_scene_info_sample_rate(nullptr) == 0U, "null info: sample_rate should be 0") && ok;
    ok = check(adm_scene_info_channels(nullptr) == 0U, "null info: channels should be 0") && ok;
    ok = check(adm_scene_info_frames(nullptr) == 0ULL, "null info: frames should be 0") && ok;
    ok = check(adm_scene_info_duration_seconds(nullptr) == 0.0, "null info: duration should be 0.0") && ok;
    ok = check(adm_scene_info_programme_count(nullptr) == 0U, "null info: programme_count should be 0") && ok;
    ok = check(adm_scene_info_object_count(nullptr) == 0U, "null info: object_count should be 0") && ok;
    return ok;
}

bool verify_progress_callback(adm_context_t* ctx, const std::filesystem::path& input) {
    // Null callback: render must still succeed without crashing.
    const auto out_null = unique_temp_wav_path("mr_c_api_prog_null");
    FileGuard g0(out_null);
    bool ok =
        check(adm_render_file(ctx, input.string().c_str(), out_null.string().c_str(), nullptr, nullptr, nullptr) ==
                  ADM_ERROR_OK,
              "render with null progress callback should succeed");

    // Full capture: verify ordering and validity of all events.
    const auto output = unique_temp_wav_path("mr_c_api_prog");
    FileGuard guard(output);
    FullProgressState state;
    const adm_error_code_t code =
        adm_render_file(ctx, input.string().c_str(), output.string().c_str(), &full_progress_cb, &state, nullptr);
    ok = check(code == ADM_ERROR_OK, "progress capture render should succeed") && ok;
    ok = check(!state.events.empty(), "at least one progress event should fire") && ok;

    const auto is_valid_stage = [](const std::string& s) {
        return s == "validating" || s == "probing" || s == "importing_scene" || s == "planning" || s == "rendering" ||
               s == "post_processing" || s == "finished";
    };
    bool fractions_ok = true;
    bool stages_ok = true;
    bool messages_ok = true;
    bool stage_enum_ok = true; // v1.7: every emitted stage string maps to a known enum
    double prev = -1.0;
    for (const auto& ev : state.events) {
        if (ev.fraction < prev || ev.fraction < 0.0 || ev.fraction > 1.0) {
            fractions_ok = false;
        }
        if (!is_valid_stage(ev.stage)) {
            stages_ok = false;
        }
        if (adm_render_stage_from_string(ev.stage.c_str()) == ADM_STAGE_UNKNOWN) {
            stage_enum_ok = false;
        }
        if (!ev.message_non_null) {
            messages_ok = false;
        }
        prev = ev.fraction;
    }
    ok = check(fractions_ok, "progress fractions should be non-decreasing and in [0, 1]") && ok;
    ok = check(stages_ok, "all progress stage strings should be from the known set") && ok;
    ok = check(stage_enum_ok, "every emitted stage string should map to a known adm_render_stage_t (no drift)") && ok;
    ok = check(messages_ok, "all progress message pointers should be non-null") && ok;
    if (!state.events.empty()) {
        ok = check(state.events.back().stage == "finished", "last progress stage should be 'finished'") && ok;
        ok = check(adm_render_stage_from_string(state.events.back().stage.c_str()) == ADM_STAGE_FINISHED,
                   "last stage should map to ADM_STAGE_FINISHED") &&
             ok;
        ok = check(state.events.back().fraction >= 0.99, "last progress fraction should reach ~1.0") && ok;
    }
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

bool verify_log_accessors(adm_context_t* ctx, const std::filesystem::path& input) {
    const auto output = unique_temp_wav_path("mr_c_api_logs");
    FileGuard guard(output);

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file(ctx, input.string().c_str(), output.string().c_str(), nullptr, nullptr, &result);
    bool ok = check(code == ADM_ERROR_OK, "log accessors: render should succeed");
    ok = check(result != nullptr, "log accessors: result must be allocated") && ok;

    const uint32_t count = adm_render_result_log_count(result);
    ok = check(count > 0, "log accessors: at least one log entry should be captured") && ok;

    // Every in-range entry must be valid; collect whether the engine module appears.
    bool all_entries_valid = true;
    bool saw_engine_module = false;
    for (uint32_t i = 0; i < count; ++i) {
        adm_log_level_t level = ADM_LOG_DEBUG;
        const char* module = nullptr;
        const char* message = nullptr;
        if (adm_render_result_log_entry(result, i, &level, &module, &message) != 1) {
            all_entries_valid = false;
            continue;
        }
        if (module == nullptr || message == nullptr) {
            all_entries_valid = false;
            continue;
        }
        if (level < ADM_LOG_DEBUG || level > ADM_LOG_ERROR) {
            all_entries_valid = false;
        }
        if (std::string{module} == "engine") {
            saw_engine_module = true;
        }
    }
    ok = check(all_entries_valid, "log accessors: every in-range entry should be valid") && ok;
    ok = check(saw_engine_module, "log accessors: at least one entry should come from the 'engine' module") && ok;

    // Out-of-range index returns 0.
    ok = check(adm_render_result_log_entry(result, count, nullptr, nullptr, nullptr) == 0,
               "log accessors: out-of-range index should return 0") &&
         ok;

    // Nullable out-params: passing all NULL still returns 1 for a valid index.
    ok = check(adm_render_result_log_entry(result, 0, nullptr, nullptr, nullptr) == 1,
               "log accessors: valid index with all-NULL out-params should return 1") &&
         ok;

    // Pointer stability: same index twice yields the same owned pointer.
    const char* msg_a = nullptr;
    const char* msg_b = nullptr;
    adm_render_result_log_entry(result, 0, nullptr, nullptr, &msg_a);
    adm_render_result_log_entry(result, 0, nullptr, nullptr, &msg_b);
    ok = check(msg_a != nullptr && msg_a == msg_b, "log accessors: message pointer should be stable") && ok;

    adm_destroy_render_result(result);

    // NULL result: count 0, entry returns 0, no crash.
    ok = check(adm_render_result_log_count(nullptr) == 0, "log accessors: NULL result log_count should be 0") && ok;
    ok = check(adm_render_result_log_entry(nullptr, 0, nullptr, nullptr, nullptr) == 0,
               "log accessors: NULL result log_entry should return 0") &&
         ok;

    // result==NULL render path (NullLogSink branch) must still succeed and not crash.
    const auto out_noresult = unique_temp_wav_path("mr_c_api_logs_noresult");
    FileGuard g2(out_noresult);
    ok = check(adm_render_file(ctx, input.string().c_str(), out_noresult.string().c_str(), nullptr, nullptr, nullptr) ==
                   ADM_ERROR_OK,
               "log accessors: render without result handle should still succeed") &&
         ok;
    return ok;
}

bool verify_inspect_json(adm_context_t* ctx, const std::filesystem::path& input) {
    // Consume the ABI as an external caller would: receive an owned JSON string
    // and inspect it via substring checks (no JSON parser pulled into this TU).
    char* json = nullptr;
    const adm_error_code_t code = adm_inspect_file_json(ctx, input.string().c_str(), &json);
    bool ok = check(code == ADM_ERROR_OK, "inspect_json: should succeed on fixture");
    ok = check(json != nullptr, "inspect_json: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        ok = check(!s.empty(), "inspect_json: JSON should be non-empty") && ok;
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        // Pin the stable schema identity so version drift is caught here.
        ok = check(has(R"("schema": "mradm.scene-inspect")") && has(R"("schema_version": 1)"),
                   "inspect_json: should carry schema + schema_version") &&
             ok;
        ok = check(has("\"programmes\"") && has("\"contents\"") && has("\"objects\""),
                   "inspect_json: should contain the top-level tree keys") &&
             ok;
        ok = check(has("TestProgramme") && has("TestContent") && has("TestObject"),
                   "inspect_json: should contain the fixture entity names") &&
             ok;
        // Fixture object block uses polar position az=30/el=10 (see write_fixture).
        ok = check(has("\"azimuth\""), "inspect_json: should contain block position fields") && ok;
    }
    adm_free_string(json);
    adm_free_string(nullptr); // must not crash

    // validate-only: out_json == NULL parses the file but allocates nothing.
    ok = check(adm_inspect_file_json(ctx, input.string().c_str(), nullptr) == ADM_ERROR_OK,
               "inspect_json: validate-only (NULL out_json) should succeed") &&
         ok;

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_inspect_file_json(nullptr, input.string().c_str(), &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_json: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_inspect_file_json(ctx, nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_json: NULL input should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_inspect_file_json(ctx, "", &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_json: empty input should be INVALID_ARGUMENT") &&
         ok;
    const auto missing = unique_temp_wav_path("mr_c_api_inspect_missing");
    ok = check(adm_inspect_file_json(ctx, missing.string().c_str(), &bad) != ADM_ERROR_OK,
               "inspect_json: missing file should fail") &&
         ok;
    ok = check(bad == nullptr, "inspect_json: out_json should stay NULL on failure") && ok;
    return ok;
}

bool verify_inspect_xml(adm_context_t* ctx, const std::filesystem::path& input) {
    char* xml = nullptr;
    const adm_error_code_t code = adm_inspect_file_xml(ctx, input.string().c_str(), &xml);
    bool ok = check(code == ADM_ERROR_OK, "inspect_xml: should succeed on fixture");
    ok = check(xml != nullptr, "inspect_xml: out_xml should be non-null") && ok;

    if (xml != nullptr) {
        const std::string s{xml};
        ok = check(!s.empty(), "inspect_xml: AXML should be non-empty") && ok;
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        // Raw ADM XML: the audioFormatExtended wrapper and the fixture entities.
        ok = check(has("audioFormatExtended"), "inspect_xml: should contain the ADM XML root") && ok;
        ok = check(has("TestObject") && has("TestProgramme"), "inspect_xml: should contain the fixture entity names") &&
             ok;
    }
    adm_free_string(xml);
    adm_free_string(nullptr); // must not crash

    // validate-only: out_xml == NULL reads the chunk but allocates nothing.
    ok = check(adm_inspect_file_xml(ctx, input.string().c_str(), nullptr) == ADM_ERROR_OK,
               "inspect_xml: validate-only (NULL out_xml) should succeed") &&
         ok;

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_inspect_file_xml(nullptr, input.string().c_str(), &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_xml: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_inspect_file_xml(ctx, nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_xml: NULL input should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_inspect_file_xml(ctx, "", &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "inspect_xml: empty input should be INVALID_ARGUMENT") &&
         ok;
    const auto missing = unique_temp_wav_path("mr_c_api_xml_missing");
    ok = check(adm_inspect_file_xml(ctx, missing.string().c_str(), &bad) != ADM_ERROR_OK,
               "inspect_xml: missing file should fail") &&
         ok;
    ok = check(bad == nullptr, "inspect_xml: out_xml should stay NULL on failure") && ok;
    return ok;
}

bool verify_policy_template_json(adm_context_t* ctx, const std::filesystem::path& input) {
    char* json = nullptr;
    const adm_error_code_t code = adm_policy_template_json(ctx, input.string().c_str(), &json);
    bool ok = check(code == ADM_ERROR_OK, "policy_template: should succeed on fixture");
    ok = check(json != nullptr, "policy_template: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        ok = check(!s.empty(), "policy_template: JSON should be non-empty") && ok;
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        // A valid policy document carrying the policy schema + per-object rule.
        ok = check(has(R"("schema": "mradm.semantic-policy.v1")"),
                   "policy_template: carries the semantic-policy schema") &&
             ok;
        ok = check(has("\"global\"") && has("\"objects\""), "policy_template: has global + objects") && ok;
        ok = check(has("TestObject"), "policy_template: includes the fixture object rule") && ok;
    }
    adm_free_string(json);

    // validate-only: out_json == NULL parses the file but allocates nothing.
    ok = check(adm_policy_template_json(ctx, input.string().c_str(), nullptr) == ADM_ERROR_OK,
               "policy_template: validate-only (NULL out_json) should succeed") &&
         ok;

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_policy_template_json(nullptr, input.string().c_str(), &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "policy_template: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_policy_template_json(ctx, nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "policy_template: NULL input should be INVALID_ARGUMENT") &&
         ok;
    const auto missing = unique_temp_wav_path("mr_c_api_tmpl_missing");
    ok = check(adm_policy_template_json(ctx, missing.string().c_str(), &bad) != ADM_ERROR_OK,
               "policy_template: missing file should fail") &&
         ok;
    ok = check(bad == nullptr, "policy_template: out_json should stay NULL on failure") && ok;
    return ok;
}

bool verify_capabilities_json(adm_context_t* ctx) {
    char* json = nullptr;
    const adm_error_code_t code = adm_capabilities_json(ctx, &json);
    bool ok = check(code == ADM_ERROR_OK, "capabilities_json: should succeed");
    ok = check(json != nullptr, "capabilities_json: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        ok = check(!s.empty(), "capabilities_json: JSON should be non-empty") && ok;
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        ok = check(has(R"("schema": "mradm.capabilities")") && has(R"("schema_version": 1)"),
                   "capabilities_json: should carry schema + schema_version") &&
             ok;
        ok = check(has("\"backends\""), "capabilities_json: should contain backends array") && ok;
        // Public renderer-selection backends should be present.
        ok = check(has(R"("renderer": "ear")") && has(R"("renderer": "saf")") && has(R"("renderer": "hoa")") &&
                       has(R"("renderer": "saf-binaural")"),
                   "capabilities_json: should list ear/saf/hoa/saf-binaural backends") &&
             ok;
        // A representative capability flag and the per-backend layouts array.
        ok = check(has("\"supports_hoa\"") && has("\"layouts\"") && has("\"channel_count\""),
                   "capabilities_json: should contain capability flags and layouts") &&
             ok;
    }
    adm_free_string(json);

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_capabilities_json(nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "capabilities_json: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_capabilities_json(ctx, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "capabilities_json: NULL out_json should be INVALID_ARGUMENT") &&
         ok;
    ok = check(bad == nullptr, "capabilities_json: out_json should stay NULL on failure") && ok;
    return ok;
}

bool verify_layouts_json(adm_context_t* ctx) {
    char* json = nullptr;
    const adm_error_code_t code = adm_layouts_json(ctx, &json);
    bool ok = check(code == ADM_ERROR_OK, "layouts_json: should succeed");
    ok = check(json != nullptr, "layouts_json: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        ok = check(!s.empty(), "layouts_json: JSON should be non-empty") && ok;
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        ok = check(has(R"("schema": "mradm.layouts")") && has(R"("schema_version": 1)"),
                   "layouts_json: should carry schema + schema_version") &&
             ok;
        ok = check(has("\"layouts\""), "layouts_json: should contain the layouts array") && ok;
        // A known row: wav 7.1.4 with its channel order and count.
        ok = check(has("L R C LFE Ls Rs Rls Rrs U+045 U-045 U+135 U-135") && has(R"("channels": 12)"),
                   "layouts_json: should contain the wav 7.1.4 channel order") &&
             ok;
        ok = check(has("\"supported_by\""), "layouts_json: should contain supported_by per row") && ok;
    }
    adm_free_string(json);
    adm_free_string(nullptr); // must not crash

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_layouts_json(nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "layouts_json: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_layouts_json(ctx, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "layouts_json: NULL out_json should be INVALID_ARGUMENT") &&
         ok;
    ok = check(bad == nullptr, "layouts_json: out_json should stay NULL on failure") && ok;
    return ok;
}

// ── v1.6 tests ────────────────────────────────────────────────────────────

bool verify_version_16() {
    return check(adm_api_version_minor() >= 6, "v1.6: minor version should be >= 6");
}

bool verify_output_formats_json(adm_context_t* ctx) {
    char* json = nullptr;
    const adm_error_code_t code = adm_output_formats_json(ctx, &json);
    bool ok = check(code == ADM_ERROR_OK, "output_formats_json: should succeed");
    ok = check(json != nullptr, "output_formats_json: out_json should be non-null") && ok;

    if (json != nullptr) {
        const std::string s{json};
        const auto has = [&s](const char* needle) { return s.find(needle) != std::string::npos; };
        ok = check(has(R"("schema": "mradm.output-formats")") && has(R"("schema_version": 1)"),
                   "output_formats_json: should carry schema + schema_version") &&
             ok;
        ok = check(has("\"features\"") && has("\"formats\""), "output_formats_json: features + formats present") && ok;
        // Every output container is enumerated.
        ok = check(has(R"("format": "wav")") && has(R"("format": "caf")") && has(R"("format": "flac")") &&
                       has(R"("format": "opus_mka")") && has(R"("format": "apac")") && has(R"("format": "iamf")") &&
                       has(R"("format": "iamf_mp4")"),
                   "output_formats_json: all containers enumerated") &&
             ok;
        // A known constraint: FLAC caps at 8 channels and rejects height layouts.
        ok = check(has(R"("max_channels": 8)") && has(R"("supports_height": false)"),
                   "output_formats_json: FLAC constraints present") &&
             ok;
        // Opus carries a per-channel bitrate range; APAC a total one.
        ok = check(has("\"bitrate_kbps_per_ch\"") && has("\"bitrate_kbps_total\""),
                   "output_formats_json: bitrate ranges present") &&
             ok;
        // APAC availability must match the build platform (macOS only).
#ifdef __APPLE__
        ok = check(has(R"("apac": true)"), "output_formats_json: apac feature true on macOS") && ok;
#else
        ok = check(has(R"("apac": false)"), "output_formats_json: apac feature false off-macOS") && ok;
        ok = check(has("\"available_reason\": \"macOS only (AudioToolbox)\""),
                   "output_formats_json: apac unavailable reason off-macOS") &&
             ok;
#endif
    }
    adm_free_string(json);

    // Error paths.
    char* bad = nullptr;
    ok = check(adm_output_formats_json(nullptr, &bad) == ADM_ERROR_INVALID_ARGUMENT,
               "output_formats_json: NULL context should be INVALID_ARGUMENT") &&
         ok;
    ok = check(adm_output_formats_json(ctx, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "output_formats_json: NULL out_json should be INVALID_ARGUMENT") &&
         ok;
    ok = check(bad == nullptr, "output_formats_json: out_json should stay NULL on failure") && ok;
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

bool file_contains_bytes(const std::filesystem::path& path, std::string_view needle) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !std::ranges::search(bytes, needle).empty();
}

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

bool verify_iamf_layer_validation(adm_context_t* ctx, const std::filesystem::path& input) {
    auto output = unique_temp_wav_path("mr_c_api_iamf_layers_invalid");
    output.replace_extension(".iamf");
    FileGuard guard(output);

    adm_render_options_t* opts = adm_create_render_options();
    adm_render_options_set_output_layout(opts, "7.1.4");
    adm_render_options_set_iamf_layers(opts, "5.1,7.1,5.1.2,7.1.4");

    adm_render_result_t* result = nullptr;
    const adm_error_code_t code =
        adm_render_file_ex(ctx, input.string().c_str(), output.string().c_str(), opts, nullptr, nullptr, &result);
    adm_destroy_render_options(opts);

    bool ok = check(result != nullptr, "IAMF layer validation: result must be allocated");
    ok = check(code == ADM_ERROR_INVALID_ARGUMENT, "IAMF layer validation: non-monotonic layers rejected") && ok;
    if (result != nullptr) {
        const char* message = adm_render_result_message(result);
        ok = check(message != nullptr && std::string(message).find("monotonic") != std::string::npos,
                   "IAMF layer validation: message should name monotonic constraint") &&
             ok;
    }
    adm_destroy_render_result(result);
    return ok;
}

} // namespace

// v1.15: realtime monitor. Tolerant of headless CI with no audio output device — the
// create may fail with a device error, in which case the playback assertions are skipped.
bool verify_monitor_abi(adm_context_t* ctx, const std::filesystem::path& input) {
    bool ok = check(adm_api_version_minor() == 16, "C ABI minor version is 16");

    // Argument validation (no device needed).
    adm_monitor_t* probe = nullptr;
    ok = check(adm_monitor_set_overrides(nullptr, nullptr, 0, 0) == ADM_ERROR_INVALID_ARGUMENT,
               "set_overrides(nullptr) rejected") &&
         ok;
    ok = check(adm_create_monitor(nullptr, input.string().c_str(), nullptr, &probe) == ADM_ERROR_INVALID_ARGUMENT,
               "create_monitor null context rejected") &&
         ok;
    ok = check(adm_create_monitor(ctx, nullptr, nullptr, &probe) == ADM_ERROR_INVALID_ARGUMENT,
               "create_monitor null input rejected") &&
         ok;
    ok = check(adm_create_monitor(ctx, input.string().c_str(), nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
               "create_monitor null out rejected") &&
         ok;
    ok = check(adm_monitor_play(nullptr) == ADM_ERROR_INVALID_ARGUMENT, "play(nullptr) rejected") && ok;
    ok = check(adm_monitor_log_count(nullptr) == 0U, "log_count(nullptr) is 0") && ok;

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

    ok = check(adm_monitor_seek(monitor, 0.0) == ADM_ERROR_OK, "monitor seek") && ok;
    ok = check(adm_monitor_set_loop(monitor, 0.0, 0.5) == ADM_ERROR_OK, "monitor set_loop") && ok;

    // Clearing overrides (NULL + count 0) is valid; a populated set echoes its revision.
    ok = check(adm_monitor_set_overrides(monitor, nullptr, 0, 0) == ADM_ERROR_OK, "monitor clear overrides") && ok;
    adm_monitor_override_t ov{};
    ov.struct_size = sizeof(adm_monitor_override_t);
    ov.object_id = "AO_1001";
    ov.gain_db = -6.0F;
    ov.diffuse_scale = 1.0F;
    ov.extent_scale = 1.0F;
    ov.divergence_scale = 1.0F;
    ok = check(adm_monitor_set_overrides(monitor, &ov, 1, 7) == ADM_ERROR_OK, "monitor set overrides") && ok;

    // Non-finite gain / scale is rejected; struct_size below the minimum is rejected.
    adm_monitor_override_t bad = ov;
    bad.gain_db = std::numeric_limits<float>::quiet_NaN();
    ok = check(adm_monitor_set_overrides(monitor, &bad, 1, 8) == ADM_ERROR_INVALID_ARGUMENT,
               "monitor set_overrides rejects NaN gain") &&
         ok;
    adm_monitor_override_t small = ov;
    small.struct_size = 4U; // below the minimum (must cover through divergence_scale)
    ok = check(adm_monitor_set_overrides(monitor, &small, 1, 9) == ADM_ERROR_INVALID_ARGUMENT,
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
            std::memcpy(raw.data() + (idx * fut_stride), &e, sizeof(e));
        };
        // Positive control: both elements valid at the future stride → accepted.
        place(0, "AO_1001", -3.0F);
        place(1, "AO_1002", -6.0F);
        const auto* arr = reinterpret_cast<const adm_monitor_override_t*>(raw.data());
        ok = check(adm_monitor_set_overrides(monitor, arr, 2, 10) == ADM_ERROR_OK,
                   "monitor set_overrides accepts future (oversized) struct_size stride") &&
             ok;
        // Poison ONLY element[1]'s gain. A correct stride reads it (NaN → reject); a buggy
        // stride (== sizeof) would read element[1] from element[0]'s zero padding and miss it.
        place(1, "AO_1002", std::numeric_limits<float>::quiet_NaN());
        ok = check(adm_monitor_set_overrides(monitor, arr, 2, 11) == ADM_ERROR_INVALID_ARGUMENT,
                   "monitor set_overrides strides by caller struct_size to reach element[1]") &&
             ok;
    }

    ok = check(adm_monitor_pause(monitor) == ADM_ERROR_OK, "monitor pause") && ok;
    ok = check(adm_monitor_log_entry(monitor, adm_monitor_log_count(monitor) + 100U, nullptr, nullptr, nullptr) == 0,
               "monitor log_entry out-of-range is 0") &&
         ok;

    adm_destroy_monitor(monitor);
    return ok;
}

int main() {
    if (!verify_version() || !verify_null_result_accessors()) {
        return EXIT_FAILURE;
    }

    adm_context_t* ctx = adm_create_context();
    if (!check(ctx != nullptr, "context creation should succeed")) {
        return EXIT_FAILURE;
    }

    bool ok = verify_error_paths(ctx);

    const FileGuard fixture(write_fixture());
    ok = verify_successful_render(ctx, fixture.path()) && ok;

    // v1.1 tests
    ok = verify_version_11() && ok;
    ok = verify_options_lifecycle() && ok;
    // v1.2 tests
    ok = verify_version_12() && ok;
    ok = verify_render_trim_setters() && ok;
    // v1.3 tests
    ok = verify_version_13() && ok;
    ok = verify_final_gain_setter() && ok;
    ok = verify_final_gain_wire_through(ctx, fixture.path()) && ok;
    // v1.4 tests
    ok = verify_version_14() && ok;
    ok = verify_cancel_token_lifecycle() && ok;
    ok = verify_cancel_pre_render(ctx, fixture.path()) && ok;
    // v1.5 tests
    ok = verify_version_15() && ok;
    ok = verify_semantic_memory_setters() && ok;
    ok = verify_semantic_policy_json_wire_through(ctx, fixture.path()) && ok;
    ok = verify_semantic_policy_json_invalid(ctx, fixture.path()) && ok;
    ok = verify_semantic_report_on_late_failure(ctx, fixture.path()) && ok;
    ok = verify_lossy_encode_failure_cleanup(ctx) && ok;
    ok = verify_render_file_ex_compat(ctx, fixture.path()) && ok;
    ok = verify_hoa_render(ctx, fixture.path()) && ok;
    ok = verify_51_render(ctx, fixture.path()) && ok;
    {
        const FileGuard fixture_1s(write_fixture_1s());
        ok = verify_loudness_metrics(ctx, fixture_1s.path()) && ok;
        ok = verify_render_trim_wire_through(ctx, fixture_1s.path()) && ok;
        ok = verify_cancel_threaded(ctx, fixture_1s.path()) && ok;
        // v1.8 tests
        ok = verify_version_18() && ok;
        ok = verify_preview_session(ctx, fixture_1s.path()) && ok;
    }
    ok = verify_probe(ctx, fixture.path()) && ok;
    ok = verify_progress_callback(ctx, fixture.path()) && ok;
    ok = verify_opus_render(ctx, fixture.path()) && ok;
    ok = verify_log_accessors(ctx, fixture.path()) && ok;
    ok = verify_inspect_json(ctx, fixture.path()) && ok;
    ok = verify_inspect_xml(ctx, fixture.path()) && ok;
    ok = verify_policy_template_json(ctx, fixture.path()) && ok;
    ok = verify_capabilities_json(ctx) && ok;
    ok = verify_layouts_json(ctx) && ok;
    // v1.6 tests
    ok = verify_version_16() && ok;
    ok = verify_output_formats_json(ctx) && ok;
    // v1.7 tests
    ok = verify_version_17() && ok;
    ok = verify_render_stage_from_string() && ok;
    // v1.9 tests
    ok = verify_version_19() && ok;
    // v1.10 tests
    ok = verify_version_110() && ok;
    ok = verify_progress_v2_render(ctx, fixture.path()) && ok;
    ok = verify_progress_v2_flac_post(ctx, fixture.path()) && ok;
    ok = verify_apac_unsupported_smoke(ctx, fixture.path()) && ok;
    ok = verify_apac_caf_smoke(ctx, fixture.path()) && ok;
    ok = verify_iamf_smoke(ctx, fixture.path()) && ok;
    // v1.11 tests
    ok = verify_version_111() && ok;
    // v1.12 tests
    ok = verify_version_112() && ok;
    ok = verify_render_support_matrix_json(ctx) && ok;
    // v1.14 tests
    ok = verify_version_114() && ok;
    ok = verify_iamf_layer_validation(ctx, fixture.path()) && ok;
    // v1.15 tests
    ok = verify_monitor_abi(ctx, fixture.path()) && ok;

    adm_destroy_context(ctx);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
