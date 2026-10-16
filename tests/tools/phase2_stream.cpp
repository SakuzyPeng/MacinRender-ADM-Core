// Release replay of file-streaming renders (PCM consistency 3.1). A binaural IRenderStream is pulled
// in a fixed request partition while live overrides, head orientations and seeks land at fixed
// output frames, so the delivered PCM is a deterministic function of the case file.
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/io.h"
#include "adm/live_override.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

#include "phase2_io.h"

namespace {
using Json = nlohmann::json;

class Log final : public mradm::LogSink {
  public:
    void log(mradm::LogLevel level, std::string_view module, std::string_view message) override {
        if (level == mradm::LogLevel::error) {
            std::cerr << module << ": " << message << '\n';
        }
    }
};

mradm::BinauralSpreadMode spread_mode(const std::string& name) {
    if (name == "none") {
        return mradm::BinauralSpreadMode::none;
    }
    if (name == "cloud") {
        return mradm::BinauralSpreadMode::cloud;
    }
    phase2::require(name == "saf-spreader", "unknown binaural spread mode");
    return mradm::BinauralSpreadMode::saf_spreader;
}

// One event's `objects` list replaces the whole live-override snapshot.
mradm::LiveOverrides overrides(const Json& objects, const mradm::AdmScene& scene, uint64_t revision) {
    mradm::LiveOverrides result;
    result.revision = revision;
    for (const auto& row : objects) {
        const auto index = row.at("index").get<std::size_t>();
        phase2::require(index < scene.objects.size(), "override object index out of range");
        mradm::LiveObjectOverride item;
        item.object_id = scene.objects[index].id;
        item.gain_db = row.value("gain_db", 0.0F);
        item.diffuse_scale = row.value("diffuse_scale", 1.0F);
        item.extent_scale = row.value("extent_scale", 1.0F);
        item.divergence_scale = row.value("divergence_scale", 1.0F);
        item.extent_width_scale = row.value("width_scale", 1.0F);
        item.extent_height_scale = row.value("height_scale", 1.0F);
        item.extent_depth_scale = row.value("depth_scale", 1.0F);
        item.mute = row.value("mute", false);
        result.objects.push_back(std::move(item));
    }
    return result;
}

std::vector<float>
replay(const Json& spec, const std::filesystem::path& input, const mradm::AdmScene& scene, uint32_t& sample_rate) {
    Log logs;
    auto renderer = mradm::create_binaural_renderer();
    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_layout = "binaural";
    plan.scene = scene;
    plan.binaural_spread_mode = spread_mode(spec.at("spread").get<std::string>());
    auto prepared = renderer->prepare(plan, logs);
    phase2::require(prepared.has_value(), "binaural prepare failed");
    auto opened = renderer->open_stream(**prepared, plan, logs);
    phase2::require(opened.has_value(), "binaural open_stream failed");
    auto stream = std::move(*opened);
    phase2::require(stream->out_channels() == 2U, "binaural stream must be stereo");
    sample_rate = stream->sample_rate();

    const auto partition = spec.at("partition").get<std::vector<std::size_t>>();
    phase2::require(!partition.empty() && std::ranges::none_of(partition, [](std::size_t n) { return n == 0U; }),
                    "invalid request partition");
    const auto& events = spec.at("events");
    const auto total = spec.at("frames").get<uint64_t>();
    std::vector<float> delivered;
    std::vector<float> block;
    uint64_t position = 0;
    uint64_t revision = 0;
    std::size_t next_event = 0;
    std::size_t request = 0;
    while (position < total) {
        while (next_event < events.size() && events[next_event].at("frame").get<uint64_t>() == position) {
            const auto& event = events[next_event++];
            if (event.contains("objects")) {
                const auto snapshot = overrides(event.at("objects"), scene, ++revision);
                phase2::require(stream->validate_overrides(snapshot).has_value(), "override rejected");
                stream->set_overrides(snapshot);
            }
            if (event.contains("orientation")) {
                const auto angles = event.at("orientation").get<std::vector<float>>();
                phase2::require(angles.size() == 3U, "orientation needs yaw, pitch and roll");
                stream->set_listener_orientation({angles[0], angles[1], angles[2]});
            }
            if (event.contains("seek")) {
                phase2::require(stream->seek(event.at("seek").get<uint64_t>()).has_value(), "seek failed");
            }
        }
        phase2::require(next_event == events.size() || events[next_event].at("frame").get<uint64_t>() > position,
                        "events must be ordered by output frame");
        const uint64_t limit = next_event < events.size() ? events[next_event].at("frame").get<uint64_t>() : total;
        const auto frames =
            static_cast<std::size_t>(std::min<uint64_t>(partition[request++ % partition.size()], limit - position));
        block.assign(frames * 2U, 0.0F);
        const auto produced = stream->process(block, frames);
        phase2::require(produced.has_value(), "stream process failed");
        delivered.insert(delivered.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*produced * 2U));
        position += *produced;
        if (*produced < frames) {
            break; // end of material
        }
    }
    phase2::require(next_event == events.size(), "replay ended before every event was applied");
    stream.reset(); // streams borrow the prepared state
    return delivered;
}
} // namespace

int main(int argc, char** argv) {
    try {
        phase2::require(argc == 4, "usage: mr_adm_phase2_stream case.json input.wav output-directory");
        std::ifstream in(argv[1]);
        const auto spec = Json::parse(in);
        const std::filesystem::path input(argv[2]);
        const std::filesystem::path directory(argv[3]);
        std::filesystem::create_directories(directory);
        auto scene = mradm::io::import_scene(input.string());
        phase2::require(scene.has_value(), "cannot import the replay fixture");
        for (int pass = 1; pass <= 2; ++pass) {
            uint32_t sample_rate = 0;
            const auto pcm = replay(spec, input, *scene, sample_rate);
            phase2::pcm(directory / ("pass-" + std::to_string(pass) + ".pcmbits"), 2U, sample_rate, pcm);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
