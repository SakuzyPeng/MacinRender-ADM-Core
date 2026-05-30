// Contract test for the stable C ABI (include/adm/c_api.h).
//
// Exercises the ABI exactly as an external consumer would: version queries,
// context/result lifecycle, every error path, and one real end-to-end render of
// a runtime-generated ADM fixture (no private audio material; see CLAUDE.md).
// The default C ABI render (no options) resolves to binaural 2ch output.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

    adm_destroy_context(ctx);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
