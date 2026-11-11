#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "adm/direct_speakers_matrix.h"
#include "adm/options.h"
#include "adm/render.h"

#include "engine_ffi.h"
#include "routing_ffi.h"

// The records production C++ hands to the Rust engine: text views, DirectSpeakers matrices, screens
// and a Rust backend preparation.
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

// A Rust backend preparation (mradm-engine `backend`): immutable and reused
// across offline windows and file streams (PreviewSession scrubbing). The Rust preparation keeps
// the input format the stream checks compare.
struct EnginePrepared final : IPreparedRender {
    std::unique_ptr<MradmRenderPrepared, decltype(&mradm_render_prepared_destroy)> handle{
        nullptr, &mradm_render_prepared_destroy};
};

// A screen as the Rust room record.
[[nodiscard]] MradmRoomScreenGeometry screen_geometry(const ScreenGeometry& screen);

} // namespace mradm::render_common
