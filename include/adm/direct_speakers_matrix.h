#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "adm/errors.h"

namespace mradm {

// One destination in a DirectSpeakers label-routing matrix. `weight` is the
// user-authored positive ratio; `gain` is its equal-power-normalised linear
// coefficient (sqrt(weight / sum(weights))) computed by the strict parser.
struct DirectSpeakersMatrixTarget {
    std::string label;
    float weight{1.0F};
    float gain{1.0F};
};

// One source-label row. A row either has one or more targets, or sets mute=true.
struct DirectSpeakersMatrixRoute {
    std::string source_label;
    std::vector<DirectSpeakersMatrixTarget> targets;
    bool mute{false};
};

struct DirectSpeakersMatrix {
    static constexpr std::string_view schema_id = "mradm.direct-speakers-matrix.v1";

    std::string output_layout;
    std::vector<DirectSpeakersMatrixRoute> routes;
};

// Parse the strict v1 JSON schema from memory. `source_label` is diagnostic
// context (for example "<memory>" or a file path).
[[nodiscard]] Result<DirectSpeakersMatrix> parse_direct_speakers_matrix(std::string_view json,
                                                                        std::string_view source_label = "<memory>");

[[nodiscard]] Result<DirectSpeakersMatrix> load_direct_speakers_matrix_file(const std::filesystem::path& path);

} // namespace mradm
