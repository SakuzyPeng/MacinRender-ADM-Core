#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/io.h"

#include "speaker_layouts.h"

namespace mradm::io {

namespace {

constexpr std::size_t k_max_input_channels = 64;

struct Preset {
    std::string_view id;
    std::string_view internal_id;
    uint32_t wave_channel_mask{0};
};

constexpr std::array<Preset, 8> k_presets{{
    {"5.1", "0+5+0", 0x003FU},
    {"5.1.2", "2+5+0", 0x503FU},
    {"7.1", "wav71", 0x063FU},
    {"5.1.4", "4+5+0", 0x2D03FU},
    {"7.1.4", "4+7+0", 0x2D63FU},
    {"9.1.4", "4+5+4", 0U},
    {"9.1.6", "9.1.6", 0U},
    {"22.2", "9+10+3", 0U},
}};

enum class AxmlState : uint8_t {
    absent,
    present,
    invalid_wave,
};

[[nodiscard]] std::string trim_upper(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    std::string out{value.substr(first, last - first + 1)};
    std::ranges::transform(
        out, out.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return out;
}

[[nodiscard]] std::string lower(std::string_view value) {
    std::string out{value};
    std::ranges::transform(
        out, out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

[[nodiscard]] bool same_position(const InputChannelDefinition& lhs, const InputChannelDefinition& rhs) {
    if (lhs.speaker_label != rhs.speaker_label || lhs.is_lfe != rhs.is_lfe) {
        return false;
    }
    if (lhs.is_lfe) {
        return true;
    }
    return std::abs(lhs.azimuth - rhs.azimuth) < 1.0e-4F && std::abs(lhs.elevation - rhs.elevation) < 1.0e-4F;
}

[[nodiscard]] std::vector<InputChannelDefinition> build_channel_catalog() {
    std::vector<InputChannelDefinition> catalog;
    for (const auto& layout : render_layouts::speaker_layouts()) {
        if (layout.id == "0+2+0") {
            continue;
        }
        for (const auto& speaker : layout.speakers) {
            InputChannelDefinition candidate{std::string{speaker.label},
                                             std::string{speaker.label},
                                             speaker.azimuth,
                                             speaker.elevation,
                                             speaker.is_lfe};
            const bool exists =
                std::ranges::any_of(catalog, [&](const auto& item) { return same_position(item, candidate); });
            if (!exists) {
                catalog.push_back(std::move(candidate));
            }
        }
    }

    for (auto& item : catalog) {
        const auto count = std::ranges::count_if(
            catalog, [&](const auto& other) { return other.speaker_label == item.speaker_label; });
        if (count > 1 && !item.is_lfe) {
            item.token = fmt::format("{}@{:.0f}", item.speaker_label, item.elevation);
        }
    }
    return catalog;
}

[[nodiscard]] uint32_t read_u32_le(std::istream& input) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return 0;
    }
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] uint64_t read_u64_le(std::istream& input) {
    std::array<unsigned char, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return 0;
    }
    uint64_t value = 0;
    unsigned int shift = 0;
    for (const auto byte : bytes) {
        value |= static_cast<uint64_t>(byte) << shift;
        shift += 8U;
    }
    return value;
}

[[nodiscard]] AxmlState scan_axml(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return AxmlState::invalid_wave;
    }

    std::array<char, 4> container{};
    std::array<char, 4> wave{};
    input.read(container.data(), 4);
    (void) read_u32_le(input);
    input.read(wave.data(), 4);
    if (!input || std::string_view{wave.data(), wave.size()} != "WAVE") {
        return AxmlState::invalid_wave;
    }
    const std::string_view kind{container.data(), container.size()};
    if (kind != "RIFF" && kind != "RF64" && kind != "BW64") {
        return AxmlState::invalid_wave;
    }

    uint64_t data_size64 = 0;
    input.seekg(0, std::ios::end);
    const auto end_pos = input.tellg();
    input.seekg(12, std::ios::beg);
    while (input && input.tellg() >= 0 && input.tellg() + std::streamoff{8} <= end_pos) {
        std::array<char, 4> id{};
        input.read(id.data(), 4);
        const uint32_t size32 = read_u32_le(input);
        if (!input) {
            break;
        }
        const std::string_view chunk_id{id.data(), id.size()};
        if (chunk_id == "axml") {
            return AxmlState::present;
        }

        const auto payload_pos = input.tellg();
        uint64_t payload_size = size32;
        if (chunk_id == "ds64" && size32 >= 24U) {
            (void) read_u64_le(input); // RIFF size
            data_size64 = read_u64_le(input);
            input.seekg(payload_pos, std::ios::beg);
        } else if (chunk_id == "data" && size32 == std::numeric_limits<uint32_t>::max() && data_size64 > 0) {
            payload_size = data_size64;
        }
        const uint64_t padded_size = payload_size + (payload_size & 1U);
        if (padded_size > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            break;
        }
        input.seekg(payload_pos + static_cast<std::streamoff>(padded_size), std::ios::beg);
    }
    return AxmlState::absent;
}

[[nodiscard]] const InputLayoutDefinition* find_input_layout(std::string_view raw) {
    const std::string key = lower(raw);
    const auto it = std::ranges::find_if(input_layouts(), [&](const auto& layout) {
        return key == lower(layout.id) || key == lower(layout.internal_id);
    });
    return it == input_layouts().end() ? nullptr : &*it;
}

struct ResolvedInputMapping {
    std::string layout;
    std::vector<InputChannelDefinition> channels;
};

[[nodiscard]] Result<void> validate_channel_bed_audio(const audio::FloatWavReader& reader, const std::string& path) {
    const uint32_t channels = reader.channels();
    if (channels == 0 || channels > k_max_input_channels) {
        return make_error(
            ErrorCode::invalid_argument,
            fmt::format("channel-bed input must contain 1-{} channels; got {}", k_max_input_channels, channels),
            "input=" + path);
    }
    const bool supported_pcm =
        reader.is_linear_pcm() &&
        (reader.bits_per_sample() == 16U || reader.bits_per_sample() == 24U || reader.bits_per_sample() == 32U);
    const bool supported_float = reader.is_ieee_float() && reader.bits_per_sample() == 32U;
    if (!supported_pcm && !supported_float) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("channel-bed input supports PCM16/24/32 or float32 WAVE; got {}-bit format",
                                      reader.bits_per_sample()),
                          "input=" + path);
    }
    if (reader.sample_rate() == 0 || reader.frame_count() == 0) {
        return make_error(ErrorCode::invalid_argument, "channel-bed input has no audio frames", "input=" + path);
    }
    return {};
}

[[nodiscard]] Result<ResolvedInputMapping>
resolve_input_mapping(const audio::FloatWavReader& reader, const SceneImportOptions& options, const std::string& path) {
    std::vector<InputChannelDefinition> resolved_channels;
    std::string resolved_layout;
    const bool explicit_layout =
        options.input_layout.has_value() && !options.input_layout->empty() && lower(*options.input_layout) != "auto";
    if (explicit_layout && !options.input_channel_labels.empty()) {
        return make_error(ErrorCode::invalid_argument,
                          "input layout and custom input-channel labels are mutually exclusive");
    }
    if (explicit_layout) {
        const auto* layout = find_input_layout(*options.input_layout);
        if (layout == nullptr) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("unknown input layout '{}'; use mradm input-layouts to list presets",
                                          *options.input_layout));
        }
        resolved_layout = layout->id;
        resolved_channels = layout->channels;
    } else if (!options.input_channel_labels.empty()) {
        auto labels = resolve_input_channel_labels(options.input_channel_labels);
        if (!labels) {
            return tl::unexpected{labels.error()};
        }
        resolved_layout = "custom";
        resolved_channels = std::move(*labels);
    } else {
        const uint32_t mask = reader.channel_mask();
        if (mask == 0U) {
            return make_error(ErrorCode::invalid_argument,
                              "channel-bed input has no recognised WAVE channel mask; pass --input-layout or "
                              "--input-channels",
                              "input=" + path);
        }
        const int mask_positions = std::popcount(mask);
        if (std::cmp_not_equal(mask_positions, reader.channels())) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("WAVE channel mask 0x{:X} contains {} positions but the file has {} channels",
                                          mask,
                                          mask_positions,
                                          reader.channels()),
                              "input=" + path);
        }
        const auto it = std::ranges::find_if(input_layouts(),
                                             [mask](const auto& layout) { return layout.wave_channel_mask == mask; });
        if (it == input_layouts().end()) {
            return make_error(ErrorCode::unsupported,
                              fmt::format("WAVE channel mask 0x{:X} is not recognised; pass --input-layout or "
                                          "--input-channels",
                                          mask),
                              "input=" + path);
        }
        resolved_layout = it->id;
        resolved_channels = it->wave_mask_channels.empty() ? it->channels : it->wave_mask_channels;
    }

    if (resolved_channels.size() != reader.channels()) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("input mapping '{}' defines {} channels but the file contains {}",
                                      resolved_layout,
                                      resolved_channels.size(),
                                      reader.channels()),
                          "input=" + path);
    }
    return ResolvedInputMapping{std::move(resolved_layout), std::move(resolved_channels)};
}

[[nodiscard]] AdmScene synthesize_channel_bed_scene(const std::string& path,
                                                    const audio::FloatWavReader& reader,
                                                    const ResolvedInputMapping& mapping,
                                                    bool ignored_axml) {
    AdmScene scene;
    scene.info.sample_rate = reader.sample_rate();
    scene.info.num_channels = static_cast<uint16_t>(reader.channels());
    scene.info.num_frames = reader.frame_count();
    scene.info.file_path = path;
    scene.info.source_kind = SceneSourceKind::channel_bed;
    scene.info.input_layout = mapping.layout;
    scene.info.input_channel_labels.reserve(mapping.channels.size());

    SceneObject object;
    object.id = "AO_CHANNEL_BED";
    object.name = "Channel Bed";
    object.end_sample = scene.info.num_frames;
    object.labels = {"channel-bed"};
    object.tracks.reserve(mapping.channels.size());
    for (std::size_t index = 0; index < mapping.channels.size(); ++index) {
        const auto& channel = mapping.channels[index];
        scene.info.input_channel_labels.push_back(channel.token);

        SceneTrackRef track;
        track.channel_index = static_cast<uint16_t>(index);
        track.track_uid = fmt::format("ATU_CHANNEL_BED_{:02}", index + 1U);
        SceneDirectSpeakersBlock block;
        block.speaker_labels = {channel.speaker_label};
        block.pack_format_id = "AP_CHANNEL_BED";
        block.azimuth = channel.azimuth;
        block.elevation = channel.elevation;
        block.distance = 1.0F;
        block.has_position = !channel.is_lfe;
        block.gain = 1.0F;
        if (channel.is_lfe) {
            block.low_pass_hz = 120.0F;
        }
        block.start_sample = 0;
        block.end_sample = scene.info.num_frames;
        track.ds_blocks.push_back(std::move(block));
        object.tracks.push_back(std::move(track));
    }
    scene.objects.push_back(std::move(object));

    SceneContent content;
    content.id = "ACO_CHANNEL_BED";
    content.name = "Channel Bed";
    content.object_ids = {"AO_CHANNEL_BED"};
    content.dialogue_kind = "non-dialogue";
    scene.contents.push_back(std::move(content));

    SceneProgramme programme;
    programme.id = "APR_CHANNEL_BED";
    programme.name = "Channel Bed";
    programme.content_ids = {"ACO_CHANNEL_BED"};
    programme.start_sample = 0;
    programme.end_sample = scene.info.num_frames;
    scene.programmes.push_back(std::move(programme));

    if (ignored_axml) {
        scene.import_warnings.emplace_back(
            "explicit channel-bed input mapping selected; embedded ADM AXML metadata was ignored");
    }
    return scene;
}

[[nodiscard]] Result<AdmScene>
make_channel_bed_scene(const std::string& path, const SceneImportOptions& options, bool ignored_axml) {
    auto reader = audio::FloatWavReader::open(path);
    if (!reader) {
        return tl::unexpected{reader.error()};
    }
    auto valid = validate_channel_bed_audio(*reader, path);
    if (!valid) {
        return tl::unexpected{valid.error()};
    }
    auto mapping = resolve_input_mapping(*reader, options, path);
    if (!mapping) {
        return tl::unexpected{mapping.error()};
    }
    return synthesize_channel_bed_scene(path, *reader, *mapping, ignored_axml);
}

} // namespace

const std::vector<InputLayoutDefinition>& input_layouts() {
    static const std::vector<InputLayoutDefinition> layouts = [] {
        std::vector<InputLayoutDefinition> out;
        out.reserve(k_presets.size());
        for (const auto& preset : k_presets) {
            const auto* source = render_layouts::find_speaker_layout(preset.internal_id);
            if (source == nullptr) {
                continue;
            }
            InputLayoutDefinition layout;
            layout.id = preset.id;
            layout.internal_id = preset.internal_id;
            layout.display_name = source->display_name;
            layout.wave_channel_mask = preset.wave_channel_mask;
            layout.channels.reserve(source->speakers.size());
            for (const auto& speaker : source->speakers) {
                layout.channels.push_back({std::string{speaker.label},
                                           std::string{speaker.label},
                                           speaker.azimuth,
                                           speaker.elevation,
                                           speaker.is_lfe});
            }
            if (layout.id == "7.1.4") {
                // Explicit presets describe file order. WAVEFORMATEXTENSIBLE
                // orders channels by ascending mask bit, so back L/R
                // (0x10/0x20) precede side L/R (0x200/0x400).
                std::swap(layout.channels[4], layout.channels[6]);
                std::swap(layout.channels[5], layout.channels[7]);
            }
            if (layout.wave_channel_mask != 0U) {
                layout.wave_mask_channels = layout.channels;
            }
            out.push_back(std::move(layout));
        }
        return out;
    }();
    return layouts;
}

const std::vector<InputChannelDefinition>& input_channel_catalog() {
    static const std::vector<InputChannelDefinition> catalog = build_channel_catalog();
    return catalog;
}

Result<std::vector<InputChannelDefinition>> resolve_input_channel_labels(const std::vector<std::string>& labels) {
    if (labels.empty() || labels.size() > k_max_input_channels) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("custom input mapping requires 1-{} labels", k_max_input_channels));
    }

    const auto& catalog = input_channel_catalog();
    std::vector<InputChannelDefinition> out;
    out.reserve(labels.size());
    std::set<std::string> used_labels;
    for (const auto& raw : labels) {
        std::string token = trim_upper(raw);
        if (token.empty()) {
            return make_error(ErrorCode::invalid_argument, "custom input mapping contains an empty label");
        }
        if (token == "L" || token == "FL") {
            token = "M+030";
        } else if (token == "R" || token == "FR") {
            token = "M-030";
        } else if (token == "C" || token == "FC") {
            token = "M+000";
        } else if (token == "LFE") {
            token = "LFE1";
        }

        std::vector<InputChannelDefinition> candidates;
        std::ranges::copy_if(catalog, std::back_inserter(candidates), [&](const auto& item) {
            return item.token == token || item.speaker_label == token;
        });
        if (candidates.empty()) {
            return make_error(
                ErrorCode::invalid_argument,
                fmt::format("unknown input channel label '{}'; use mradm input-layouts for the catalog", raw));
        }
        if (candidates.size() > 1U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("input channel label '{}' has multiple nominal elevations; use '{}@30' or "
                                          "'{}@45'",
                                          token,
                                          token,
                                          token));
        }
        auto resolved = std::move(candidates.front());
        if (!used_labels.insert(resolved.speaker_label).second) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("duplicate input speaker label '{}'", resolved.speaker_label));
        }
        out.push_back(std::move(resolved));
    }
    return out;
}

Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options) {
    const bool explicit_layout =
        options.input_layout.has_value() && !options.input_layout->empty() && lower(*options.input_layout) != "auto";
    const bool forced_channel_bed = explicit_layout || !options.input_channel_labels.empty();
    const AxmlState axml = scan_axml(path);
    if (axml == AxmlState::invalid_wave) {
        return make_error(ErrorCode::io_error, "input is not a RIFF/RF64/BW64 WAVE file", "input=" + path);
    }
    if (!forced_channel_bed && axml == AxmlState::present) {
        return import_scene(path);
    }
    return make_channel_bed_scene(path, options, forced_channel_bed && axml == AxmlState::present);
}

} // namespace mradm::io
