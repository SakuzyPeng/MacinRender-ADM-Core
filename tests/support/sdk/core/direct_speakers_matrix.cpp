#include "adm/direct_speakers_matrix.h"

#include <memory>
#include <string>

#include "config_test_ffi.h"
#include "direct_matrix_records.h"

namespace mradm {

Result<DirectSpeakersMatrix> parse_direct_speakers_matrix(std::string_view json, std::string_view source_label) {
    // A message is fixed text plus at most one decoded substring of the input, so it always fits.
    std::string message(json.size() + 1024, '\0');
    auto message_len = message.size();
    MradmDirectMatrix* raw = nullptr;
    MradmDirectMatrixView view{};
    const auto code = mradm_scene_direct_matrix_parse(
        reinterpret_cast<const uint8_t*>(json.data()), json.size(), &raw, &view, message.data(), &message_len);
    if (code != 0) {
        message.resize(message_len);
        return make_error(static_cast<ErrorCode>(code), std::move(message), std::string{source_label});
    }
    const std::unique_ptr<MradmDirectMatrix, decltype(&mradm_scene_direct_matrix_destroy)> handle(
        raw, &mradm_scene_direct_matrix_destroy);
    return detail::matrix_from_view(view);
}

} // namespace mradm
