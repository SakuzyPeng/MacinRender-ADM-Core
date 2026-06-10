#pragma once

#include <string>

#include "adm/errors.h"
#include "adm/scene.h"

namespace mradm::io {

// Import an ADM BWF file from path and return a self-owned AdmScene.
// Returns io_error if the file is missing, not a valid BW64/ADM file,
// or the axml chunk is absent.
Result<AdmScene> import_scene(const std::string& path);

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
// Stage 1 covers Objects (object gain/mute; block gain/position/diffuse/extent/
// divergence/channelLock/jumpPosition) and DirectSpeakers (gain, spherical
// position). HOA pack gain/mute and block interpolationLength are not written
// back yet; differences there are ignored.
Result<void> write_scene(const std::string& src_path,
                         const AdmScene& original,
                         const AdmScene& effective,
                         const std::string& dst_path);

} // namespace mradm::io
