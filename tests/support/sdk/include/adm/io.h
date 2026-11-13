#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "adm/errors.h"

namespace mradm::io {

// How RenderService imports an input: ADM BWF, or an ordinary channel-based WAVE/RF64/BW64 file
// from which a DirectSpeakers scene is synthesized.
struct SceneImportOptions {
    // nullopt / "auto" selects ADM when AXML exists, otherwise a recognised
    // WAVEFORMATEXTENSIBLE channel mask. Any other value forces channel-bed input.
    std::optional<std::string> input_layout;
    // Custom labels in file-channel order. Mutually exclusive with an explicit layout.
    std::vector<std::string> input_channel_labels;
};

struct InputChannelDefinition {
    std::string token;
    std::string speaker_label;
    float azimuth{0.0F};
    float elevation{0.0F};
    bool is_lfe{false};
};

struct InputLayoutDefinition {
    std::string id;
    std::string internal_id;
    std::string display_name;
    uint32_t wave_channel_mask{0}; // 0 = no automatic WAVE mask mapping
    // Exact file order for an explicit preset. Presets with a WAVE mask use
    // ascending mask-bit order for both explicit and automatic detection.
    std::vector<InputChannelDefinition> channels;
    std::vector<InputChannelDefinition> wave_mask_channels;
};

// Single source of truth for accepted channel-bed presets and custom labels.
const std::vector<InputLayoutDefinition>& input_layouts();
const std::vector<InputChannelDefinition>& input_channel_catalog();
Result<std::vector<InputChannelDefinition>> resolve_input_channel_labels(const std::vector<std::string>& labels);

// Return the raw AXML chunk content as a UTF-8 string without parsing.
Result<std::string> get_axml(const std::string& path);

} // namespace mradm::io
