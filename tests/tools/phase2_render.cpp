// Repeat the manifest's exact RenderService requests twice in one process.
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"

#include "decimal.h"
#include "phase2_io.h"

namespace {
// The same locale-independent, correctly rounded parser as the mradm command line (PCM consistency 3.5).
template <typename T> T decimal(const std::string& text) {
    const auto value = mradm::dsp::parse_decimal<T>(text);
    if (!value) {
        throw std::runtime_error("invalid decimal option value: " + text);
    }
    return *value;
}
using Json = nlohmann::json;
class Log final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel /*level*/, std::string_view module, std::string_view message) override {
        std::cout << module << ": " << message << '\n';
    }
};
mradm::RenderOptions options(const Json& args) {
    mradm::RenderOptions result;
    result.output_bit_depth = mradm::OutputBitDepth::f32;
    const auto values = args.get<std::vector<std::string>>();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& key = values[i];
        if (key == "--no-peak-limit") {
            result.peak_limit = false;
            continue;
        }
        if (key == "--peak-normalize-to-limit") {
            result.peak_normalize_to_limit = true;
            continue;
        }
        phase2::require(i + 1 < values.size(), "missing option value");
        const auto& v = values[++i];
        if (key == "--renderer") {
            static const std::map<std::string, mradm::RendererSelection> names{
                {"ear", mradm::RendererSelection::ear},
                {"saf", mradm::RendererSelection::saf},
                {"saf-binaural", mradm::RendererSelection::saf_binaural},
                {"hoa", mradm::RendererSelection::hoa},
                {"triple-balance", mradm::RendererSelection::triple_balance}};
            result.renderer = names.at(v);
        } else if (key == "--output-layout") {
            result.output_layout = v;
        } else if (key == "--object-smoothing-frames") {
            result.object_smoothing_frames = static_cast<uint32_t>(std::stoul(v));
        } else if (key == "--start") {
            result.render_start_sec = decimal<double>(v);
        } else if (key == "--end") {
            result.render_end_sec = decimal<double>(v);
        } else if (key == "--speaker-spread-mode" && v == "none") {
            result.speaker_spread_mode = mradm::SpeakerSpreadMode::none;
        } else if (key == "--binaural-spread-mode") {
            phase2::require(v == "cloud" || v == "saf-spreader", "unsupported spread mode");
            result.binaural_spread_mode =
                v == "cloud" ? mradm::BinauralSpreadMode::cloud : mradm::BinauralSpreadMode::saf_spreader;
        } else if (key == "--listener-yaw") {
            result.listener_orientation.yaw_deg = decimal<float>(v);
        } else if (key == "--listener-pitch") {
            result.listener_orientation.pitch_deg = decimal<float>(v);
        } else if (key == "--listener-roll") {
            result.listener_orientation.roll_deg = decimal<float>(v);
        } else if (key == "--loudness-target") {
            result.measure_loudness = true;
            result.loudness_target_lufs = decimal<float>(v);
        } else if (key == "--peak-limit-dbtp") {
            result.peak_limit_dbtp = decimal<float>(v);
        } else if (key == "--triple-balance-mode") {
            phase2::require(v == "standard" || v == "d", "unsupported Triple Balance mode");
            result.triple_balance_mode = v == "d" ? mradm::TripleBalanceMode::d : mradm::TripleBalanceMode::standard;
        } else {
            throw std::runtime_error("unsupported measurement option: " + key);
        }
    }
    return result;
}
} // namespace
int main(int argc, char** argv) {
    try {
        phase2::require(argc == 4, "usage: mr_adm_phase2_render case.json input.wav output-directory");
        std::ifstream in(argv[1]);
        const auto spec = Json::parse(in);
        const std::filesystem::path directory(argv[3]);
        std::filesystem::create_directories(directory);
        mradm::RenderService service;
        mradm::NullProgressSink progress;
        Log logs;
        for (int pass = 1; pass <= 2; ++pass) {
            mradm::RenderRequest request;
            request.input_path = argv[2];
            request.output_path = directory / ("pass-" + std::to_string(pass) + ".wav");
            request.options = options(spec.at("args"));
            if (spec.contains("sofa")) {
                const std::filesystem::path name(spec.at("sofa").get<std::string>());
                phase2::require(name == name.filename() && name.extension() == ".sofa", "invalid SOFA fixture name");
                request.options.sofa_path =
                    std::filesystem::path(argv[1]).parent_path().parent_path() / "fixtures" / name;
            }
            const auto result = service.render(request, progress, logs);
            if (!result.success()) {
                throw std::runtime_error(result.error.message);
            }
            auto reader = mradm::audio::FloatWavReader::open(request.output_path->string());
            phase2::require(reader.has_value(), "cannot open repeat output");
            std::vector<float> samples(static_cast<std::size_t>(reader->frame_count()) * reader->channels());
            phase2::require(reader->read(samples.data(), reader->frame_count()) == reader->frame_count(),
                            "short repeat output");
            phase2::pcm(directory / ("pass-" + std::to_string(pass) + ".pcmbits"),
                        reader->channels(),
                        reader->sample_rate(),
                        samples);
            // Close before unlinking on Windows.
            reader = mradm::make_error(mradm::ErrorCode::io_error, "closed");
            std::filesystem::remove(*request.output_path);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
