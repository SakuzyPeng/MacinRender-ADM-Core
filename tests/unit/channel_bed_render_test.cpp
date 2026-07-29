#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"
#include "adm/render.h"

#include "test_portable.h"

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

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
    }
    return condition;
}

bool write_fixture(const std::filesystem::path& path, uint32_t channels) {
    constexpr uint32_t k_sample_rate = 48000;
    constexpr uint64_t k_frames = 4096;
    std::vector<float> samples(k_frames * channels);
    for (uint64_t frame = 0; frame < k_frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            constexpr float k_frequency = 80.0F;
            const float phase =
                static_cast<float>(frame) * k_frequency * 6.28318530718F / static_cast<float>(k_sample_rate);
            samples[(frame * channels) + channel] = 0.01F * static_cast<float>(channel + 1U) * std::sin(phase);
        }
    }

    auto writer = mradm::audio::FloatWavWriter::open(path.string(), channels, k_sample_rate);
    if (!writer) {
        std::cerr << writer.error().message << "\n";
        return false;
    }
    return writer->write(samples.data(), k_frames) == k_frames;
}

bool verify_output(const std::filesystem::path& path, uint32_t expected_channels) {
    auto reader = mradm::audio::FloatWavReader::open(path.string());
    if (!check(reader.has_value(), "open rendered channel-bed output")) {
        return false;
    }
    bool ok = check(reader->channels() == expected_channels, "rendered output channel count") &&
              check(reader->sample_rate() == 48000U, "rendered output sample rate") &&
              check(reader->frame_count() == 4096U, "rendered output frame count");
    std::vector<float> samples(reader->frame_count() * reader->channels());
    const uint64_t frames_read = reader->read(samples.data(), reader->frame_count());
    ok = check(frames_read == reader->frame_count(), "read complete rendered output") && ok;
    double magnitude = 0.0;
    for (float sample : samples) {
        if (!std::isfinite(sample)) {
            return check(false, "rendered output samples are finite");
        }
        magnitude += std::abs(static_cast<double>(sample));
    }
    return check(magnitude > 0.01, "rendered output contains signal") && ok;
}

std::vector<double> channel_rms(const std::filesystem::path& path) {
    auto reader = mradm::audio::FloatWavReader::open(path.string());
    if (!reader) {
        return {};
    }
    std::vector<float> samples(reader->frame_count() * reader->channels());
    if (reader->read(samples.data(), reader->frame_count()) != reader->frame_count()) {
        return {};
    }
    std::vector<double> rms(reader->channels(), 0.0);
    for (uint64_t frame = 0; frame < reader->frame_count(); ++frame) {
        for (uint32_t channel = 0; channel < reader->channels(); ++channel) {
            const double sample = samples[(frame * reader->channels()) + channel];
            rms[channel] += sample * sample;
        }
    }
    std::ranges::transform(rms, rms.begin(), [frame_count = reader->frame_count()](const double value) {
        return std::sqrt(value / static_cast<double>(frame_count));
    });
    return rms;
}

std::string container_id(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, 4> id{};
    input.read(id.data(), static_cast<std::streamsize>(id.size()));
    return input ? std::string{id.data(), id.size()} : std::string{};
}

mradm::RenderResult render(const std::filesystem::path& input,
                           const std::filesystem::path& output,
                           mradm::RendererSelection renderer,
                           std::string layout,
                           std::vector<std::string> labels = {},
                           std::string input_layout = "5.1",
                           mradm::OutputBitDepth bit_depth = mradm::OutputBitDepth::f32) {
    mradm::RenderRequest request;
    request.input_path = input;
    request.output_path = output;
    request.options.renderer = renderer;
    request.options.output_layout = std::move(layout);
    request.options.output_bit_depth = bit_depth;
    request.options.peak_limit = false;
    if (labels.empty()) {
        request.options.input_layout = std::move(input_layout);
    } else {
        request.options.input_channel_labels = std::move(labels);
    }

    mradm::RenderService service;
    mradm::NullProgressSink progress;
    mradm::NullLogSink logs;
    return service.render(request, progress, logs);
}

} // namespace

// NOLINTNEXTLINE(readability-function-size): one integration flow verifies every WAV layout-semantic family.
int main() {
    FileGuard input{mr_test::temp_prefix() + "channel_bed_render_input.wav"};
    FileGuard multichannel{mr_test::temp_prefix() + "channel_bed_render_51.wav"};
    FileGuard output_512{mr_test::temp_prefix() + "channel_bed_render_512.wav"};
    FileGuard output_71{mr_test::temp_prefix() + "channel_bed_render_71.wav"};
    FileGuard output_514{mr_test::temp_prefix() + "channel_bed_render_514.wav"};
    FileGuard binaural{mr_test::temp_prefix() + "channel_bed_render_binaural.wav"};
    FileGuard input_714{mr_test::temp_prefix() + "channel_bed_render_input_714.wav"};
    FileGuard output_714{mr_test::temp_prefix() + "channel_bed_render_output_714.wav"};
    FileGuard output_914{mr_test::temp_prefix() + "channel_bed_render_output_914.wav"};
    FileGuard output_916{mr_test::temp_prefix() + "channel_bed_render_output_916.wav"};
    FileGuard output_222{mr_test::temp_prefix() + "channel_bed_render_output_222.wav"};
    FileGuard output_hoa3{mr_test::temp_prefix() + "channel_bed_render_output_hoa3.wav"};
    if (!check(write_fixture(input.path(), 6U), "write ordinary float32 input fixture") ||
        !check(write_fixture(input_714.path(), 12U), "write 7.1.4 input fixture")) {
        return EXIT_FAILURE;
    }

    const auto speaker_result = render(input.path(), multichannel.path(), mradm::RendererSelection::ear, "5.1");
    bool ok =
        check(speaker_result.success(), "ordinary float32 input renders through EAR: " + speaker_result.error.message);
    if (speaker_result.success()) {
        ok = verify_output(multichannel.path(), 6U) && ok;
        auto reader = mradm::audio::FloatWavReader::open(multichannel.path().string());
        ok = check(reader.has_value() && reader->channel_mask() == 0x003FU,
                   "5.1 WAV carries WAVEFORMATEXTENSIBLE mask 0x3F") &&
             ok;
    }

    const auto verify_mask_layout = [&](const std::filesystem::path& output,
                                        const std::string& layout,
                                        uint32_t expected_channels,
                                        uint32_t expected_mask) {
        const auto result = render(input.path(), output, mradm::RendererSelection::ear, layout);
        bool layout_ok = check(result.success(), layout + " channel bed renders: " + result.error.message);
        if (result.success()) {
            auto reader = mradm::audio::FloatWavReader::open(output.string());
            layout_ok = check(reader.has_value() && reader->channels() == expected_channels &&
                                  reader->channel_mask() == expected_mask,
                              layout + " WAV carries the expected WAVEFORMATEXTENSIBLE mask") &&
                        layout_ok;
        }
        return layout_ok;
    };
    ok = verify_mask_layout(output_512.path(), "5.1.2", 8U, 0x503FU) && ok;
    ok = verify_mask_layout(output_71.path(), "7.1", 8U, 0x063FU) && ok;
    ok = verify_mask_layout(output_514.path(), "5.1.4", 10U, 0x2D03FU) && ok;

    const auto binaural_result = render(input.path(),
                                        binaural.path(),
                                        mradm::RendererSelection::saf_binaural,
                                        "binaural",
                                        {"R", "L", "C", "LFE", "M-110", "M+110"});
    ok = check(binaural_result.success(),
               "custom ordinary input renders through SAF binaural: " + binaural_result.error.message) &&
         ok;
    if (binaural_result.success()) {
        ok = verify_output(binaural.path(), 2U) && ok;
        auto reader = mradm::audio::FloatWavReader::open(binaural.path().string());
        ok = check(reader.has_value() && reader->channel_mask() == 0U,
                   "binaural WAV does not claim loudspeaker positions") &&
             ok;
        auto axml = mradm::io::get_axml(binaural.path().string());
        ok = check(axml.has_value() && axml->find("typeDefinition=\"Binaural\"") != std::string::npos &&
                       axml->find("leftEar") != std::string::npos && axml->find("rightEar") != std::string::npos,
                   "binaural WAV carries ADM Binaural leftEar/rightEar semantics") &&
             ok;
        auto scene = mradm::io::import_scene(binaural.path().string());
        ok = check(scene.has_value() && scene->objects.size() == 1U && scene->objects[0].tracks.size() == 2U &&
                       scene->objects[0].tracks[0].channel_index == 0U &&
                       scene->objects[0].tracks[1].channel_index == 1U,
                   "float32 binaural ADM/CHNA round-trips through the importer") &&
             ok;
    }

    const auto result_714 = render(input_714.path(),
                                   output_714.path(),
                                   mradm::RendererSelection::ear,
                                   "7.1.4",
                                   {},
                                   "7.1.4",
                                   mradm::OutputBitDepth::i24);
    ok = check(result_714.success(), "7.1.4 channel bed renders: " + result_714.error.message) && ok;
    if (result_714.success()) {
        auto reader = mradm::audio::FloatWavReader::open(output_714.path().string());
        ok = check(reader.has_value() && reader->channel_mask() == 0x2D63FU, "7.1.4 WAV carries mask 0x2D63F") && ok;
        const auto input_rms = channel_rms(input_714.path());
        const auto output_rms = channel_rms(output_714.path());
        ok = check(input_rms.size() == 12U && output_rms.size() == 12U, "7.1.4 RMS vectors are complete") && ok;
        if (input_rms.size() == output_rms.size()) {
            const double render_scale = output_rms[0] / input_rms[0];
            for (std::size_t channel = 0; channel < input_rms.size(); ++channel) {
                const double expected = input_rms[channel] * render_scale;
                ok = check(std::abs(expected - output_rms[channel]) < std::max(2.0e-4, expected * 0.02),
                           "7.1.4 file channel " + std::to_string(channel + 1U) +
                               " preserves its speaker signal after WAVE-order rewrite") &&
                     ok;
            }
        }
    }

    const auto result_914 = render(
        input.path(), output_914.path(), mradm::RendererSelection::ear, "9.1.4", {}, "5.1", mradm::OutputBitDepth::i24);
    ok = check(result_914.success(), "9.1.4 channel bed renders: " + result_914.error.message) && ok;
    if (result_914.success()) {
        ok = check(container_id(output_914.path()) == "BW64", "integer 9.1.4 ADM output uses BW64") && ok;
        auto scene = mradm::io::import_scene(output_914.path().string());
        ok = check(scene.has_value() && scene->info.num_channels == 14U && scene->objects.size() == 1U &&
                       scene->objects[0].tracks.size() == 14U && !scene->objects[0].tracks[0].ds_blocks.empty() &&
                       !scene->objects[0].tracks[0].ds_blocks[0].speaker_labels.empty() &&
                       scene->objects[0].tracks[0].ds_blocks[0].speaker_labels[0] == "M+030" &&
                       !scene->objects[0].tracks[13].ds_blocks.empty() &&
                       !scene->objects[0].tracks[13].ds_blocks[0].speaker_labels.empty() &&
                       scene->objects[0].tracks[13].ds_blocks[0].speaker_labels[0] == "U-150",
                   "9.1.4 ADM DirectSpeakers geometry and CHNA round-trip") &&
             ok;
    }

    const auto verify_direct_speakers_layout = [&](const std::filesystem::path& output,
                                                   const std::string& layout,
                                                   uint32_t expected_channels,
                                                   const std::string& expected_last_label) {
        const auto result =
            render(input.path(), output, mradm::RendererSelection::ear, layout, {}, "5.1", mradm::OutputBitDepth::i24);
        bool layout_ok = check(result.success(), layout + " channel bed renders: " + result.error.message);
        if (result.success()) {
            auto scene = mradm::io::import_scene(output.string());
            layout_ok = check(container_id(output) == "BW64" && scene.has_value() &&
                                  scene->info.num_channels == expected_channels && scene->objects.size() == 1U &&
                                  scene->objects[0].tracks.size() == expected_channels &&
                                  !scene->objects[0].tracks.back().ds_blocks.empty() &&
                                  !scene->objects[0].tracks.back().ds_blocks[0].speaker_labels.empty() &&
                                  scene->objects[0].tracks.back().ds_blocks[0].speaker_labels[0] == expected_last_label,
                              layout + " PCM BW64 carries complete ADM DirectSpeakers semantics") &&
                        layout_ok;
        }
        return layout_ok;
    };
    ok = verify_direct_speakers_layout(output_916.path(), "9.1.6", 16U, "U-150") && ok;
    ok = verify_direct_speakers_layout(output_222.path(), "22.2", 24U, "B-045") && ok;

    const auto result_hoa3 = render(input.path(), output_hoa3.path(), mradm::RendererSelection::hoa, "hoa3", {}, "5.1");
    ok = check(result_hoa3.success(), "HOA3 channel bed renders: " + result_hoa3.error.message) && ok;
    if (result_hoa3.success()) {
        auto scene = mradm::io::import_scene(output_hoa3.path().string());
        ok = check(scene.has_value() && scene->info.num_channels == 16U && scene->hoa_tracks.size() == 1U &&
                       scene->hoa_tracks[0].normalization == "SN3D" && scene->hoa_tracks[0].channels.size() == 16U &&
                       scene->hoa_tracks[0].channels[1].order == 1 && scene->hoa_tracks[0].channels[1].degree == -1,
                   "float32 HOA3 ADM ACN/SN3D semantics and CHNA round-trip") &&
             ok;
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
