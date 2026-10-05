#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <adm/parse.hpp>
#include <bw64/bw64.hpp>
#include <nlohmann/json.hpp>

#include "adm/io.h"

#include "metadata.h"
#include "scene_json.h"
#include "speaker_layouts.h"

namespace mradm::io::libadm_reference {
Result<AdmScene> import_scene(const std::string& path);
}
namespace {
using nlohmann::json;
json gain(const mradm::SceneGainSource& s) {
    return {{"present", s.present}, {"db", s.decibels}, {"value", s.value}, {"linear", s.linear}};
}
json block_source(const std::optional<mradm::SceneBlockSource>& s) {
    if (!s) {
        return nullptr;
    }
    return {{"gain", gain(s->gain)},
            {"rtime_present", s->rtime_present},
            {"rtime", s->rtime_samples},
            {"duration", s->duration_samples ? json(*s->duration_samples) : json(nullptr)}};
}
json snapshot(const mradm::AdmScene& s) {
    auto output = json::parse(mradm::engine::scene_to_json(s));
    output["import_warnings"] = s.import_warnings;
    auto provenance = json::array();
    for (const auto& o : s.objects) {
        json v;
        if (o.adm_source) {
            const auto& p = *o.adm_source;
            v["object"] = {{"gain", gain(p.gain)},
                           {"mute_present", p.mute_present},
                           {"mute", p.mute},
                           {"start_present", p.start_present},
                           {"start", p.start_samples},
                           {"absolute_start", p.absolute_start_samples},
                           {"children", p.child_objects},
                           {"has_parent", p.has_parent},
                           {"duration", p.duration_samples ? json(*p.duration_samples) : json(nullptr)}};
        }
        for (const auto& t : o.tracks) {
            json track;
            for (const auto& b : t.blocks) {
                track["objects"].push_back(block_source(b.adm_source));
            }
            for (const auto& b : t.ds_blocks) {
                auto source = block_source(b.adm_source);
                if (b.source_position) {
                    const auto& p = *b.source_position;
                    source["position"] = {p.cartesian, p.azimuth, p.elevation, p.distance, p.x, p.y, p.z};
                }
                track["direct"].push_back(source);
            }
            v["tracks"].push_back(track);
        }
        provenance.push_back(v);
    }
    output["provenance"] = provenance;
    return output;
}
bool equivalent(const json& reference, const json& actual, const std::string& path = "") {
    if (reference.type() != actual.type() || reference.size() != actual.size()) {
        std::cerr << "type/size mismatch: " << path << '\n';
        return false;
    }
    if (reference.is_object()) {
        for (const auto& [key, value] : reference.items()) {
            if (!actual.contains(key) || !equivalent(value, actual.at(key), path + "/" + key)) {
                return false;
            }
        }
        return true;
    }
    if (reference.is_array()) {
        for (std::size_t i = 0; i < reference.size(); ++i) {
            if (!equivalent(reference[i], actual[i], path + "/" + std::to_string(i))) {
                return false;
            }
        }
        return true;
    }
    if (reference.is_number_float()) {
        const auto a = reference.get<double>();
        const auto b = actual.get<double>();
        if (std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(a))) {
            return true;
        }
    } else if (reference == actual) {
        return true;
    }
    std::cerr << "value mismatch: " << path << " reference=" << reference << " actual=" << actual << '\n';
    return false;
}
bool compare(const std::string& path) {
    const auto start = std::chrono::steady_clock::now();
    const auto baseline = mradm::io::libadm_reference::import_scene(path);
    const auto midpoint = std::chrono::steady_clock::now();
    const auto rust = mradm::io::import_scene(path);
    const auto end = std::chrono::steady_clock::now();
    if (!baseline || !rust) {
        std::cerr << path << ": " << (!baseline ? baseline.error().message : rust.error().message) << '\n';
        return false;
    }
    if (!equivalent(snapshot(*baseline), snapshot(*rust))) {
        return false;
    }
    std::cout << json{{"input", std::filesystem::path(path).filename().string()},
                      {"equivalent", true},
                      {"libadm_ms", std::chrono::duration<double, std::milli>(midpoint - start).count()},
                      {"rust_ms", std::chrono::duration<double, std::milli>(end - midpoint).count()}}
                     .dump()
              << '\n';
    return true;
}
bool generated(const std::filesystem::path& directory) {
    for (const std::string kind : {"binaural", "hoa3", "7.1.4", "9.1.6"}) {
        const auto* layout = mradm::render_layouts::find_speaker_layout(kind == "7.1.4" ? "4+7+0" : kind);
        const uint32_t type = kind == "binaural" ? 5U : kind == "hoa3" ? 4U : 1U;
        auto result = mradm::metadata::generate(
            type, kind, layout ? layout->speakers : std::vector<mradm::render_layouts::SpeakerSpec>{});
        if (!result) {
            std::cerr << result.error().message << '\n';
            return false;
        }
        std::istringstream stream{result->axml};
        if (!adm::parseXml(stream)) {
            return false;
        }
        auto chna = std::make_shared<bw64::ChnaChunk>();
        for (const auto& entry : result->chna) {
            chna->addAudioId(bw64::AudioId{entry.track_index, entry.track_uid, entry.track_format, entry.pack_format});
        }
        const auto path = directory / ("generated-" + kind + ".wav");
        auto axml = std::make_shared<bw64::AxmlChunk>(result->axml);
        {
            auto writer =
                bw64::writeFile(path.string(), static_cast<uint16_t>(result->chna.size()), 48000, 24, chna, axml);
            std::vector<float> samples(result->chna.size() * 8U, 0.0F);
            writer->write(samples.data(), 8);
        }
        if (!compare(path.string())) {
            return false;
        }
    }
    return true;
}
} // namespace
int main(int argc, char** argv) {
    try {
        const std::vector<std::string> arguments(argv, argv + argc);
        if (arguments.size() < 2) {
            return 2;
        }
        if (!generated(arguments[1])) {
            return 1;
        }
        for (std::size_t i = 2; i < arguments.size(); ++i) {
            if (!compare(arguments[i])) {
                return 1;
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
