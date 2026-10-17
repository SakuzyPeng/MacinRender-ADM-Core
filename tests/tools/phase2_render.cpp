// Repeat the manifest's exact RenderService requests twice in one process.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/room.h"
#include "adm/screen.h"

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
                {"triple-balance", mradm::RendererSelection::triple_balance},
                {"room-binaural", mradm::RendererSelection::room_binaural}};
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
        } else if (key == "--final-gain-db") {
            result.final_gain_db = decimal<double>(v);
        } else if (key == "--output-bit-depth") {
            static const std::map<std::string, mradm::OutputBitDepth> depths{{"f32", mradm::OutputBitDepth::f32},
                                                                             {"i24", mradm::OutputBitDepth::i24},
                                                                             {"i16", mradm::OutputBitDepth::i16}};
            result.output_bit_depth = depths.at(v);
        } else if (key == "--direct-speakers-routing") {
            phase2::require(v == "matrix", "unsupported DirectSpeakers routing");
            result.direct_speakers_routing_mode = mradm::DirectSpeakersRoutingMode::matrix;
        } else if (key == "--room-quality") {
            phase2::require(v == "exact" || v == "realtime", "unsupported room quality");
            result.room.quality = v == "realtime" ? mradm::RoomQuality::realtime : mradm::RoomQuality::exact;
        } else if (key == "--triple-balance-mode") {
            phase2::require(v == "standard" || v == "d", "unsupported Triple Balance mode");
            result.triple_balance_mode = v == "d" ? mradm::TripleBalanceMode::d : mradm::TripleBalanceMode::standard;
        } else {
            throw std::runtime_error("unsupported measurement option: " + key);
        }
    }
    return result;
}
// Fixture files named by the case (PCM consistency 4.2): the same RenderOptions fields the mradm
// command line fills from these flags, resolved inside the collection's fixtures directory.
void apply_files(const Json& files, const std::filesystem::path& fixtures, mradm::RenderOptions& render_options) {
    for (const auto& [flag, value] : files.items()) {
        const std::filesystem::path name(value.get<std::string>());
        phase2::require(name.is_relative() && name.lexically_normal() == name && *name.begin() != "..",
                        "invalid fixture file name");
        const auto path = fixtures / name;
        if (flag == "--semantic-policy") {
            render_options.semantic_policy_path = path;
        } else if (flag == "--direct-speakers-matrix") {
            render_options.direct_speakers_matrix_path = path;
        } else if (flag == "--screen-config") {
            auto screens = mradm::load_screen_config(path);
            phase2::require(screens.has_value(), "cannot load screen configuration");
            render_options.screens = *screens;
        } else if (flag == "--room") {
            auto room = mradm::RoomProfile::load(path);
            phase2::require(room.has_value(), "cannot load room profile");
            render_options.room.profile = *room;
        } else if (flag == "--room-hrir") {
            render_options.room.hrir_manifest = path;
        } else {
            throw std::runtime_error("unsupported fixture file option: " + flag);
        }
    }
}
// The final file's samples as float32. Integer WAV must keep the requested depth; 16- and 24-bit
// samples convert to float32 exactly, so equal floats mean equal integers.
template <typename Reader> std::vector<float> samples(Reader& reader) {
    std::vector<float> result(static_cast<std::size_t>(reader.frame_count()) * reader.channels());
    phase2::require(reader.read(result.data(), reader.frame_count()) == reader.frame_count(), "short repeat output");
    return result;
}
uint16_t bits(mradm::OutputBitDepth depth) {
    switch (depth) {
    case mradm::OutputBitDepth::i16:
        return 16U;
    case mradm::OutputBitDepth::i24:
        return 24U;
    case mradm::OutputBitDepth::f32:
        break;
    }
    return 32U;
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
            // Optional final container: "caf" writes float32 CAF instead of WAV (PCM consistency 3.6).
            const bool caf = spec.value("container", std::string{"wav"}) == "caf";
            const auto output = directory / ("pass-" + std::to_string(pass) + (caf ? ".caf" : ".wav"));
            request.output_path = output;
            request.options = options(spec.at("args"));
            if (spec.contains("sofa")) {
                const std::filesystem::path name(spec.at("sofa").get<std::string>());
                phase2::require(name == name.filename() && name.extension() == ".sofa", "invalid SOFA fixture name");
                request.options.sofa_path =
                    std::filesystem::path(argv[1]).parent_path().parent_path() / "fixtures" / name;
            }
            if (spec.contains("files")) {
                apply_files(spec.at("files"),
                            std::filesystem::path(argv[1]).parent_path().parent_path() / "fixtures",
                            request.options);
            }
            const auto result = service.render(request, progress, logs);
            if (!result.success()) {
                throw std::runtime_error(result.error.message);
            }
            const auto bits_path = directory / ("pass-" + std::to_string(pass) + ".pcmbits");
            if (caf) {
                auto reader = mradm::audio::FloatCafReader::open(output.string());
                phase2::require(reader.has_value(), "cannot open repeat output");
                phase2::pcm(bits_path, reader->channels(), reader->sample_rate(), samples(*reader));
                // Close before unlinking on Windows.
                reader = mradm::make_error(mradm::ErrorCode::io_error, "closed");
            } else {
                auto reader = mradm::audio::FloatWavReader::open(output.string());
                phase2::require(reader.has_value(), "cannot open repeat output");
                const bool floating = request.options.output_bit_depth == mradm::OutputBitDepth::f32;
                phase2::require(reader->is_ieee_float() == floating &&
                                    reader->bits_per_sample() == bits(request.options.output_bit_depth),
                                "repeat output has the requested sample format");
                phase2::pcm(bits_path, reader->channels(), reader->sample_rate(), samples(*reader));
                reader = mradm::make_error(mradm::ErrorCode::io_error, "closed");
            }
            std::filesystem::remove(output);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
