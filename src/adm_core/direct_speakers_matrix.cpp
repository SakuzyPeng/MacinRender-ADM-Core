#include "adm/direct_speakers_matrix.h"

#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "config_ffi.h"

namespace mradm {
namespace {

[[nodiscard]] Error io_matrix(const std::filesystem::path& path, std::string message) {
    return {ErrorCode::io_error, "DirectSpeakers matrix I/O error: " + std::move(message), path.string()};
}

[[nodiscard]] std::string text(MradmAdmText value) {
    return {reinterpret_cast<const char*>(value.data), value.len};
}

} // namespace

// Parsed in Rust mradm-scene `direct_matrix` (PCM consistency 4.4).
Result<DirectSpeakersMatrix> parse_direct_speakers_matrix(std::string_view json, std::string_view source_label) {
    // A message is fixed text plus at most one decoded substring of the input, so it always fits.
    std::vector<uint8_t> message(json.size() + 1024);
    MradmDirectMatrix* raw = nullptr;
    MradmDirectMatrixView view{};
    const auto code = mradm_scene_direct_matrix_parse(
        reinterpret_cast<const uint8_t*>(json.data()), json.size(), &raw, &view, message.data(), message.size());
    if (code != 0) {
        return make_error(
            static_cast<ErrorCode>(code), reinterpret_cast<const char*>(message.data()), std::string{source_label});
    }
    const std::unique_ptr<MradmDirectMatrix, decltype(&mradm_scene_direct_matrix_destroy)> handle(
        raw, &mradm_scene_direct_matrix_destroy);
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

Result<DirectSpeakersMatrix> load_direct_speakers_matrix_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return tl::unexpected{io_matrix(path, "cannot open file")};
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return tl::unexpected{io_matrix(path, "failed while reading file")};
    }
    return parse_direct_speakers_matrix(buffer.str(), path.string());
}

} // namespace mradm
