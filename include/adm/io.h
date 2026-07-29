#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm::io {

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

// Import an ADM BWF file from path and return a self-owned AdmScene.
// Returns io_error if the file is missing, not a valid BW64/ADM file,
// or the axml chunk is absent.
Result<AdmScene> import_scene(const std::string& path);

// Rendering-oriented import. In addition to ADM BWF, accepts an ordinary
// channel-based WAVE/RF64/BW64 file and synthesizes a DirectSpeakers scene.
Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options);

// Single source of truth for accepted channel-bed presets and custom labels.
const std::vector<InputLayoutDefinition>& input_layouts();
const std::vector<InputChannelDefinition>& input_channel_catalog();
Result<std::vector<InputChannelDefinition>> resolve_input_channel_labels(const std::vector<std::string>& labels);

// Return the raw AXML chunk content as a UTF-8 string without parsing.
Result<std::string> get_axml(const std::string& path);

// Write a new ADM BW64 file at dst_path by re-serializing the source document
// with the semantic differences between original and effective applied.
//
// PCM and the chna chunk are copied byte-for-byte from src_path via a chunk-level
// RIFF/BW64 rewrite (no sample decode/encode), so the audio is bit-exact and the
// sample rate / bit depth are irrelevant. Only the ADM metadata fields that differ
// between original and effective are patched into the regenerated axml, keeping
// every ADM element the domain model does not capture intact instead of rebuilding
// a lossy document.
//
// original and effective MUST both originate from import_scene(src_path)
// (effective optionally transformed by apply_semantic_policy), so their
// object/track/block ordering aligns one-to-one with the source document.
//
// Stage 1 covers Objects (object gain/mute; block gain/diffuse/extent/
// divergence/channelLock/jumpPosition/interpolationLength) and DirectSpeakers
// gain. Position and HOA pack gain/mute are not written back yet; differences
// there are ignored.
Result<void> write_scene(const std::string& src_path,
                         const AdmScene& original,
                         const AdmScene& effective,
                         const std::string& dst_path);

} // namespace mradm::io
