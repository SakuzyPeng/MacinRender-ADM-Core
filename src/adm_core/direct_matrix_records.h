#pragma once

#include <span>
#include <string>

#include "adm/direct_speakers_matrix.h"

#include "config_ffi.h"

namespace mradm::detail {

// A copy of a Rust DirectSpeakers matrix view (mradm-scene `direct_matrix`).
[[nodiscard]] inline DirectSpeakersMatrix matrix_from_view(const MradmDirectMatrixView& view) {
    const auto text = [](MradmAdmText value) {
        return value.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(value.data), value.len};
    };
    DirectSpeakersMatrix matrix;
    matrix.output_layout = text(view.output_layout);
    matrix.routes.reserve(view.routes_len);
    for (const auto& record : std::span{view.routes, view.routes_len}) {
        auto& route = matrix.routes.emplace_back();
        route.source_label = text(record.source_label);
        route.mute = record.mute != 0U;
        route.targets.reserve(record.targets_len);
        for (const auto& target : std::span{record.targets, record.targets_len}) {
            route.targets.push_back({text(target.label), target.weight, target.gain});
        }
    }
    return matrix;
}

} // namespace mradm::detail
