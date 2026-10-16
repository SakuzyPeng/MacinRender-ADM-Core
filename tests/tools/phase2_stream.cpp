// Release replay of file-streaming renders (PCM consistency 3.1 binaural, 3.2 EAR). The backend
// comes from the renderer factory shared with RenderService and the monitor; its IRenderStream is
// pulled in a fixed request partition while live overrides, head orientations and seeks land at
// fixed output frames, so the delivered PCM is a deterministic function of the case file.
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/io.h"
#include "adm/live_override.h"
#include "adm/render.h"

#include "phase2_io.h"
#include "renderer_factory.h"

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

mradm::RendererSelection renderer(const std::string& name) {
    static const std::map<std::string, mradm::RendererSelection> names{
        {"ear", mradm::RendererSelection::ear}, {"saf-binaural", mradm::RendererSelection::saf_binaural}};
    phase2::require(names.contains(name), "unsupported file-streaming renderer");
    return names.at(name);
}

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
        item.speaker_label = row.value("speaker_label", std::string{});
        item.mute = row.value("mute", false);
        result.objects.push_back(std::move(item));
    }
    return result;
}

std::vector<float>
replay(const Json& spec, const std::filesystem::path& input, const mradm::AdmScene& scene, uint32_t& sample_rate) {
    phase2::require(spec.at("version").get<int>() == 3, "unsupported file-streaming replay version");
    phase2::require(scene.info.num_frames == spec.at("input_frames").get<uint64_t>(),
                    "file-streaming input frame count differs from the case");
    phase2::require(scene.info.sample_rate == spec.at("sample_rate").get<uint32_t>(),
                    "file-streaming input sample rate differs from the case");
    Log logs;
    auto resolved = mradm::resolve_renderer(renderer(spec.at("renderer").get<std::string>()),
                                            mradm::normalize_output_layout(spec.at("output_layout").get<std::string>()),
                                            false);
    phase2::require(resolved.has_value(), "cannot resolve the file-streaming renderer");
    mradm::RenderPlan plan;
    plan.input_path = input.string();
    plan.output_layout = resolved->effective_output_layout;
    plan.scene = scene;
    if (spec.contains("spread")) {
        plan.binaural_spread_mode = spread_mode(spec.at("spread").get<std::string>());
    }
    auto prepared = resolved->renderer->prepare(plan, logs);
    phase2::require(prepared.has_value(), "prepare failed");
    auto opened = resolved->renderer->open_stream(**prepared, plan, logs);
    phase2::require(opened.has_value(), "open_stream failed");
    auto stream = std::move(*opened);
    const auto channels = stream->out_channels();
    phase2::require(channels == spec.at("channels").get<uint32_t>(), "stream channel count differs from the case");
    sample_rate = stream->sample_rate();
    phase2::require(sample_rate == scene.info.sample_rate, "file-streaming output sample rate differs from the input");

    const auto partition = spec.at("partition").get<std::vector<std::size_t>>();
    phase2::require(!partition.empty() && std::ranges::none_of(partition, [](std::size_t n) { return n == 0U; }),
                    "invalid request partition");
    const auto& events = spec.at("events");
    const auto total = spec.at("frames").get<uint64_t>();
    std::vector<float> delivered;
    std::vector<float> block;
    uint64_t position = 0;
    uint64_t media_position = 0;
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
                const auto target = event.at("seek").get<uint64_t>();
                phase2::require(stream->seek(target).has_value(), "seek failed");
                media_position = std::min(target, scene.info.num_frames);
            }
        }
        phase2::require(next_event == events.size() || events[next_event].at("frame").get<uint64_t>() > position,
                        "events must be ordered by output frame");
        const uint64_t limit =
            next_event < events.size() ? std::min(total, events[next_event].at("frame").get<uint64_t>()) : total;
        const auto frames =
            static_cast<std::size_t>(std::min<uint64_t>(partition[request++ % partition.size()], limit - position));
        block.assign(frames * channels, 0.0F);
        const auto produced = stream->process(block, frames);
        phase2::require(produced.has_value(), "stream process failed");
        // Check the media timeline independently of the stream's reported progress. In particular,
        // a seek that reports success without moving must not pass as a shorter, repeatable replay.
        const auto expected = std::min<uint64_t>(frames, scene.info.num_frames - media_position);
        phase2::require(*produced == expected, "file-streaming frame count differs from replay timeline");
        delivered.insert(
            delivered.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*produced * channels));
        position += *produced;
        media_position += *produced;
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
            phase2::pcm(directory / ("pass-" + std::to_string(pass) + ".pcmbits"),
                        spec.at("channels").get<uint32_t>(),
                        sample_rate,
                        pcm);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
