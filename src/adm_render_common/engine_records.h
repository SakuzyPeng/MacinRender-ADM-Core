#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "adm/direct_speakers_matrix.h"

#include "routing_ffi.h"

// Text views and DirectSpeakers matrix records passed to the Rust Apple bus planner.
namespace mradm::render_common {

[[nodiscard]] inline MradmAdmText text(std::string_view value) noexcept {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// A DirectSpeakers matrix as Rust routing records, borrowing the matrix's labels.
struct MatrixRecords {
    explicit MatrixRecords(const DirectSpeakersMatrix& matrix);
    std::vector<std::vector<MradmRenderMatrixTarget>> targets;
    std::vector<MradmRenderMatrixRoute> routes;
    std::size_t label_bytes{0};
};

} // namespace mradm::render_common
