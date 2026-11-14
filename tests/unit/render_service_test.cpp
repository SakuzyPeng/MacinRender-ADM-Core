#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "adm/render.h"
#include "adm/render_service.h"

#include "../support/wav_fixture.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Files {
  public:
    Files()
        : directory_(
              std::filesystem::temp_directory_path() /
              ("mr_render_service_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        require(std::filesystem::create_directory(directory_), "create test directory");
        auto writer = fixture::write_wave(path("input.wav").string(), 6U, 48000U, 24U, nullptr, nullptr);
        const std::vector<float> samples(std::size_t{6} * 64U, 0.1F);
        writer->write(samples.data(), 64U);
    }
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
    Files(Files&&) = delete;
    Files& operator=(Files&&) = delete;
    ~Files() { std::filesystem::remove_all(directory_); }

    [[nodiscard]] std::filesystem::path path(std::string_view name) const { return directory_ / std::string{name}; }

    [[nodiscard]] mradm::RenderRequest request() const {
        mradm::RenderRequest request;
        request.input_path = path("input.wav");
        request.output_path = path("output.wav");
        request.options.input_layout = "5.1";
        request.options.output_layout = "0+5+0";
        request.options.renderer = mradm::RendererSelection::saf;
        request.options.peak_limit = false;
        return request;
    }

  private:
    std::filesystem::path directory_;
};

class Progress final : public mradm::ProgressSink {
  public:
    void on_progress(const mradm::ProgressEvent& event) override {
        if (event.operation == mradm::RenderOperation::prepare_backend) {
            ++preparations_;
        }
    }
    [[nodiscard]] unsigned preparations() const { return preparations_; }

  private:
    unsigned preparations_{0};
};

void verify_cached_preparation(mradm::RendererSelection renderer) {
    const Files files;
    auto request = files.request();
    request.options.renderer = renderer;
    request.options.capture_semantic_report = true;
    const mradm::RenderService service;
    mradm::NullLogSink logs;
    Progress progress;
    std::shared_ptr<mradm::IPreparedRender> cache;
    const auto first = service.render(request, progress, logs, nullptr, &cache);
    require(first.success() && cache != nullptr, "first render fills the preparation cache");
    const auto retained = cache;
    const auto second = service.render(request, progress, logs, nullptr, &cache);
    require(second.success(), "second render with an independently cached preparation succeeds");
    require(progress.preparations() == 1U, "backend preparation runs once without a preimported scene");
    require(cache == retained, "the existing preparation is retained");
    require(second.semantic_report_json.has_value(), "a reused preparation still captures the semantic report");
}

void verify_policy_errors() {
    const Files files;
    auto options = files.request().options;
    options.semantic_policy_json = "{";
    const mradm::RenderService service;
    mradm::NullLogSink logs;
    const auto malformed = service.prepare_preview_scene(files.path("input.wav"), options, logs);
    require(!malformed && malformed.error().code == mradm::ErrorCode::invalid_argument,
            "invalid policy JSON returns a parameter error");
    options.semantic_policy_json.reset();
    options.semantic_policy_path = files.path("missing.json");
    const auto missing = service.prepare_preview_scene(files.path("input.wav"), options, logs);
    require(!missing && missing.error().code == mradm::ErrorCode::io_error, "a missing policy returns an I/O error");
}

#ifndef _WIN32
void verify_invalid_utf8_paths() {
    const Files files;
    auto request = files.request();
    request.output_path = files.path("output-\xff.wav");
    const auto replacement = files.path("output-\xef\xbf\xbd.wav");
    constexpr std::string_view sentinel{"unrelated existing file"};
    {
        std::ofstream existing(replacement, std::ios::binary);
        existing << sentinel;
    }
    const mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    const auto invalid = service.render(request, progress, logs);
    require(!invalid.success() && invalid.error.code == mradm::ErrorCode::invalid_argument,
            "invalid UTF-8 output path returns a parameter error");
    std::ifstream existing(replacement, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>{existing}, std::istreambuf_iterator<char>{}};
    require(content == sentinel, "invalid UTF-8 cannot redirect a write to a replacement-character filename");
    require(!std::filesystem::exists(*request.output_path), "invalid output path was not created");

    request.output_path = replacement;
    require(service.render(request, progress, logs).success(), "a literal UTF-8 replacement character is valid");

    auto options = files.request().options;
    options.semantic_policy_path = files.path("policy-\xff.json");
    const auto policy = service.prepare_preview_scene(files.path("input.wav"), options, logs);
    require(!policy && policy.error().code == mradm::ErrorCode::invalid_argument,
            "invalid UTF-8 policy path returns a parameter error");
}
#endif

} // namespace

int main() try {
    verify_cached_preparation(mradm::RendererSelection::saf);
#ifdef __APPLE__
    verify_cached_preparation(mradm::RendererSelection::apple);
#endif
    verify_policy_errors();
#ifndef _WIN32
    verify_invalid_utf8_paths();
#endif
    std::cout << "render service regression tests passed\n";
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
}
