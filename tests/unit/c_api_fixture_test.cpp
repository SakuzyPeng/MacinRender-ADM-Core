// Contract test for the stable C ABI (include/adm/c_api.h).
//
// Exercises the ABI exactly as an external consumer would: version queries,
// context/result lifecycle, every error path, and one real end-to-end render of
// a runtime-generated ADM fixture (no private audio material; see CLAUDE.md).
// The default C ABI render (no options) resolves to binaural 2ch output.

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
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
std::filesystem::path write_fixture() {
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
    auto writer = bw64::writeFile(path.string(), 1U, 48000U, 24U, chna, axml);
    constexpr uint64_t k_frames = 9600U; // 0.2 s @ 48 kHz
    std::vector<float> samples(k_frames, 0.5F);
    writer->write(samples.data(), k_frames);
    return path;
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
    return check(adm_api_version_minor() == 1, "v1.1: minor version should be 1");
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
    ok = check(adm_render_options_set_loudness_target(nullptr, -23.0) == ADM_ERROR_OK,
               "NULL opts set_loudness_target should return OK") &&
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
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 12001) == ADM_ERROR_INVALID_ARGUMENT,
               "apac_bitrate 12001 (> 12000) should return INVALID_ARGUMENT") &&
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
    ok = check(adm_render_options_set_apac_bitrate_kbps(opts, 12000) == ADM_ERROR_OK,
               "apac_bitrate 12000 (boundary) should return OK") &&
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
    ok = check(adm_render_options_set_output_layout(opts, "7.1.4") == ADM_ERROR_OK, "valid layout should return OK") &&
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

} // namespace

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
    ok = verify_render_file_ex_compat(ctx, fixture.path()) && ok;
    ok = verify_hoa_render(ctx, fixture.path()) && ok;
    ok = verify_51_render(ctx, fixture.path()) && ok;
    {
        const FileGuard fixture_1s(write_fixture_1s());
        ok = verify_loudness_metrics(ctx, fixture_1s.path()) && ok;
    }
    ok = verify_probe(ctx, fixture.path()) && ok;

    adm_destroy_context(ctx);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
