#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "adm/io.h"
#include "adm/scene.h"
#include "adm/semantic_policy.h"

#include "commands.h"

namespace {

void print_loudness(const mradm::SceneLoudnessMetadata& lm) {
    if (const auto loudness_method = lm.loudness_method; loudness_method.has_value()) {
        fmt::print("    loudness method: {}\n", loudness_method.value());
    }
    if (const auto integrated_loudness = lm.integrated_loudness; integrated_loudness.has_value()) {
        fmt::print("    integrated loudness: {:.1f} LUFS\n", integrated_loudness.value());
    }
    if (const auto max_true_peak = lm.max_true_peak; max_true_peak.has_value()) {
        fmt::print("    max true peak:       {:.1f} dBTP\n", max_true_peak.value());
    }
    if (const auto loudness_range = lm.loudness_range; loudness_range.has_value()) {
        fmt::print("    loudness range:      {:.1f} LU\n", loudness_range.value());
    }
    if (const auto max_momentary = lm.max_momentary; max_momentary.has_value()) {
        fmt::print("    max momentary:       {:.1f} LUFS\n", max_momentary.value());
    }
    if (const auto max_short_term = lm.max_short_term; max_short_term.has_value()) {
        fmt::print("    max short-term:      {:.1f} LUFS\n", max_short_term.value());
    }
    if (const auto dialogue_loudness = lm.dialogue_loudness; dialogue_loudness.has_value()) {
        fmt::print("    dialogue loudness:   {:.1f} LUFS\n", dialogue_loudness.value());
    }
}

std::string_view dialogue_id_name(unsigned int dialogue_id) {
    switch (dialogue_id) {
    case 0:
        return "non-dialogue";
    case 1:
        return "dialogue";
    case 2:
        return "mixed";
    default:
        return {};
    }
}

void print_programmes(const std::vector<mradm::SceneProgramme>& programmes) {
    fmt::print("\nProgrammes ({}):\n", programmes.size());
    for (const auto& p : programmes) {
        fmt::print("  {}  \"{}\"\n", p.id, p.name);
        if (const auto language = p.language; language.has_value()) {
            fmt::print("    language: {}\n", language.value());
        }
        if (!p.labels.empty()) {
            fmt::print("    label:");
            for (const auto& l : p.labels) {
                fmt::print(" \"{}\"", l);
            }
            fmt::print("\n");
        }
        if (p.start_sample > 0) {
            fmt::print("    start: {} samples\n", p.start_sample);
        }
        if (const auto end_sample = p.end_sample; end_sample.has_value()) {
            fmt::print("    end:   {} samples\n", end_sample.value());
        }
        if (p.has_reference_screen) {
            fmt::print("    reference screen: present (geometry not parsed by libadm)\n");
        }
        if (const auto loudness = p.loudness; loudness.has_value()) {
            print_loudness(loudness.value());
        }
        for (const auto& cid : p.content_ids) {
            fmt::print("    → {}\n", cid);
        }
    }
}

void print_content_metadata(const mradm::SceneContent& c) {
    if (const auto language = c.language; language.has_value()) {
        fmt::print("    language: {}\n", language.value());
    }
    if (!c.labels.empty()) {
        fmt::print("    label:");
        for (const auto& l : c.labels) {
            fmt::print(" \"{}\"", l);
        }
        fmt::print("\n");
    }
    if (const auto dialogue_kind = c.dialogue_kind; dialogue_kind.has_value()) {
        if (const auto content_kind = c.content_kind; content_kind.has_value()) {
            fmt::print("    dialogue: {} ({})\n", dialogue_kind.value(), content_kind.value());
        } else {
            fmt::print("    dialogue: {}\n", dialogue_kind.value());
        }
    }
    if (const auto loudness = c.loudness; loudness.has_value()) {
        print_loudness(loudness.value());
    }
}

void print_contents(const std::vector<mradm::SceneContent>& contents) {
    fmt::print("\nContents ({}):\n", contents.size());
    for (const auto& c : contents) {
        fmt::print("  {}  \"{}\"\n", c.id, c.name);
        print_content_metadata(c);
        for (const auto& oid : c.object_ids) {
            fmt::print("    → {}\n", oid);
        }
    }
}

void print_scene(const std::string& path, const mradm::AdmScene& scene) {
    const auto& info = scene.info;
    fmt::print("File: {}\n", path);
    fmt::print(
        "  Sample rate: {} Hz  Channels: {}  Frames: {}\n", info.sample_rate, info.num_channels, info.num_frames);
    if (info.sample_rate > 0 && info.num_frames > 0) {
        fmt::print("  Duration:    {:.2f} s\n", static_cast<double>(info.num_frames) / info.sample_rate);
    }

    print_programmes(scene.programmes);
    print_contents(scene.contents);

    fmt::print("\nObjects ({}):\n", scene.objects.size());
    for (const auto& obj : scene.objects) {
        fmt::print("  {}  \"{}\"\n", obj.id, obj.name);
        if (!obj.labels.empty()) {
            fmt::print("    label:");
            for (const auto& l : obj.labels) {
                fmt::print(" \"{}\"", l);
            }
            fmt::print("\n");
        }
        if (const auto importance = obj.importance; importance.has_value()) {
            fmt::print("    importance: {}\n", importance.value());
        }
        if (const auto dialogue_id = obj.dialogue_id; dialogue_id.has_value()) {
            const auto dialogue_name = dialogue_id_name(dialogue_id.value());
            if (!dialogue_name.empty()) {
                fmt::print("    dialogue: {}\n", dialogue_name);
            } else {
                fmt::print("    dialogue: {}\n", dialogue_id.value());
            }
        }
        for (const auto& track : obj.tracks) {
            if (track.channel_index.has_value()) {
                fmt::print("    {}  ch={}\n", track.track_uid, *track.channel_index);
            } else {
                fmt::print("    {}  ch=<unmapped>\n", track.track_uid);
            }
            for (std::size_t bi = 0; bi < track.blocks.size(); ++bi) {
                const auto& blk = track.blocks[bi];
                if (blk.position.cartesian) {
                    fmt::print("      block[{}]: x={:.3f} y={:.3f} z={:.3f}  gain={:.3f}\n",
                               bi,
                               blk.position.x,
                               blk.position.y,
                               blk.position.z,
                               blk.gain);
                } else {
                    fmt::print("      block[{}]: az={:.1f} el={:.1f} dist={:.3f}  gain={:.3f}\n",
                               bi,
                               blk.position.azimuth,
                               blk.position.elevation,
                               blk.position.distance,
                               blk.gain);
                }
                if (blk.diffuse > 0.0F || blk.width > 0.0F || blk.height > 0.0F) {
                    fmt::print("               diffuse={:.3f}  w={:.1f} h={:.1f} d={:.1f}\n",
                               blk.diffuse,
                               blk.width,
                               blk.height,
                               blk.depth);
                }
            }
            for (std::size_t bi = 0; bi < track.ds_blocks.size(); ++bi) {
                const auto& blk = track.ds_blocks[bi];
                fmt::print("      ds_block[{}]: labels=", bi);
                if (blk.speaker_labels.empty()) {
                    fmt::print("<none>");
                } else {
                    for (std::size_t li = 0; li < blk.speaker_labels.size(); ++li) {
                        fmt::print("{}{}", li == 0 ? "" : ",", blk.speaker_labels[li]);
                    }
                }
                fmt::print(
                    "  pack={}  gain={:.3f}\n", blk.pack_format_id.empty() ? "<unset>" : blk.pack_format_id, blk.gain);
                if (blk.has_position) {
                    fmt::print("                   az={:.1f} el={:.1f} dist={:.3f}\n",
                               blk.azimuth,
                               blk.elevation,
                               blk.distance);
                }
            }
        }
    }
}

} // namespace

CLI::App* add_inspect_command(CLI::App& app, InspectCliOptions& opts) {
    auto* inspect_cmd = app.add_subcommand("inspect", "Print ADM scene metadata from a BWF file");
    inspect_cmd->add_option("file", opts.input, "ADM BWF/WAV input path")->required();
    inspect_cmd->add_flag("--xml", opts.xml, "Dump raw AXML chunk instead of parsed summary");
    inspect_cmd->add_option("--write-semantic-policy-template",
                            opts.semantic_policy_template_path,
                            "Write editable ADM semantic policy JSON template for this input");
    return inspect_cmd;
}

int run_inspect(const InspectCliOptions& opts) {
    if (opts.xml && !opts.semantic_policy_template_path.empty()) {
        spdlog::error("--xml cannot be combined with --write-semantic-policy-template");
        return EXIT_FAILURE;
    }
    if (opts.xml) {
        auto result = mradm::io::get_axml(opts.input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        fmt::print("{}", result.value());
    } else if (!opts.semantic_policy_template_path.empty()) {
        auto result = mradm::io::import_scene(opts.input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        auto write_result =
            mradm::write_semantic_policy_template_file(opts.semantic_policy_template_path, result.value());
        if (!write_result.has_value()) {
            spdlog::error("{}", write_result.error().message);
            return EXIT_FAILURE;
        }
        spdlog::info("wrote {}", opts.semantic_policy_template_path);
    } else {
        auto result = mradm::io::import_scene(opts.input);
        if (!result.has_value()) {
            spdlog::error("{}", result.error().message);
            return EXIT_FAILURE;
        }
        print_scene(opts.input, result.value());
    }
    return EXIT_SUCCESS;
}
