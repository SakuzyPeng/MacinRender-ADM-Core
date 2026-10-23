#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"

#include "metadata.h"
#include "scene_ffi.h"
#include "scene_handle.h"
#include "scene_import_internal.h"
#include "wav_backend.h"

namespace mradm::io {

// The import itself is Rust (mradm-scene, PCM consistency 4.3): WAVE format, the CHNA channel policy,
// the ADM projection and channel-bed synthesis. This adapter builds the public scene from its view.
namespace {

MradmAdmText text(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

std::string copy(MradmAdmText value) {
    return value.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(value.data), value.len};
}

Error scene_error(int32_t code, const std::array<uint8_t, 1024>& message, uint32_t context, const std::string& path) {
    const auto error = code >= 1 && code <= 6 ? static_cast<ErrorCode>(code) : ErrorCode::internal_error;
    std::string locator;
    if (context == 1U) {
        locator = "input=" + path;
    } else if (context == 2U) {
        locator = "path=" + path;
    }
    return {error, reinterpret_cast<const char*>(message.data()), std::move(locator)};
}

std::vector<MradmAdmText> texts(const std::vector<std::string>& values) {
    std::vector<MradmAdmText> out;
    out.reserve(values.size());
    std::ranges::transform(values, std::back_inserter(out), [](const auto& value) { return text(value); });
    return out;
}

std::string message_text(const std::array<uint8_t, 1024>& message) {
    return reinterpret_cast<const char*>(message.data());
}

} // namespace

Result<RenderScene>
RenderScene::import(const std::string& path, const SceneImportOptions* options, const char* checkpoint) {
    const auto utf8_path = audio::wav_path_utf8(path);
    const auto labels = options != nullptr ? texts(options->input_channel_labels) : std::vector<MradmAdmText>{};
    const bool has_layout = options != nullptr && options->input_layout.has_value();
    const auto layout = has_layout ? text(*options->input_layout) : MradmAdmText{};
    std::array<uint8_t, 1024> message{};
    uint32_t context = 0;
    MradmSceneHandle* raw = nullptr;
    const auto code = mradm_scene_import(reinterpret_cast<const uint8_t*>(utf8_path.data()),
                                         utf8_path.size(),
                                         options != nullptr ? 1U : 0U,
                                         has_layout ? 1U : 0U,
                                         layout,
                                         labels.data(),
                                         labels.size(),
                                         &raw,
                                         &context,
                                         message.data(),
                                         message.size());
    RenderScene scene{std::make_shared<detail::RenderSceneHandle>(raw), path};
    if (code != 0) {
        return tl::unexpected{scene_error(code, message, context, path)};
    }
    if (checkpoint != nullptr) {
        scene.checkpoint(checkpoint);
    }
    if (auto refreshed = scene.refresh(); !refreshed) {
        return tl::unexpected{refreshed.error()};
    }
    return scene;
}

Result<void> RenderScene::refresh() {
    std::array<uint8_t, 1024> message{};
    MradmSceneImportInfo info{};
    MradmAdmView view{};
    MradmSceneOverlay overlay{};
    auto code = mradm_scene_view(handle_->raw, &info, &view, message.data(), message.size());
    if (code == 0) {
        code = mradm_scene_overlay(handle_->raw, &overlay, message.data(), message.size());
    }
    if (code != 0) {
        return tl::unexpected{scene_error(code, message, 1U, path_)};
    }
    const bool channel_bed = info.source_kind == 1U;
    auto refreshed_scene = metadata::scene_from_view(view, !channel_bed, &overlay);
    if (!refreshed_scene) {
        auto error = refreshed_scene.error();
        error.context = "input=" + path_;
        return tl::unexpected{std::move(error)};
    }
    auto& scene_info = refreshed_scene->info;
    scene_info.sample_rate = info.sample_rate;
    scene_info.num_channels = static_cast<uint16_t>(info.channels);
    scene_info.num_frames = info.frames;
    scene_info.file_path = path_;
    scene_info.source_kind = channel_bed ? SceneSourceKind::channel_bed : SceneSourceKind::adm;
    scene_info.input_layout = copy(info.input_layout);
    for (const auto& label : std::span{info.input_channel_labels, info.input_channel_labels_len}) {
        scene_info.input_channel_labels.push_back(copy(label));
    }
    scene_ = std::move(*refreshed_scene);
    return {};
}

void RenderScene::checkpoint(const char* stage) const {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    // Diagnostic only; a failed write is detected by the collector's checkpoint inventory.
    std::array<uint8_t, 1024> message{};
    (void) mradm_scene_checkpoint(handle_->raw, text(stage), message.data(), message.size());
#else
    (void) stage;
#endif
}

Result<void> RenderScene::apply_policy(std::string_view json, const std::string& label) {
    std::array<uint8_t, 1024> message{};
    uint32_t context = 0;
    const auto code = mradm_scene_apply_policy(handle_->raw,
                                               reinterpret_cast<const uint8_t*>(json.data()),
                                               json.size(),
                                               &context,
                                               message.data(),
                                               message.size());
    if (code != 0) {
        auto error = scene_error(code, message, context, path_);
        if (context == 3U) {
            error.context = label;
        }
        return tl::unexpected{std::move(error)};
    }
    return refresh();
}

std::vector<std::string> RenderScene::policy_warnings() const {
    std::array<uint8_t, 1024> message{};
    const MradmAdmText* warnings = nullptr;
    size_t count = 0;
    std::vector<std::string> out;
    if (mradm_scene_policy_warnings(handle_->raw, &warnings, &count, message.data(), message.size()) == 0) {
        for (const auto& warning : std::span{warnings, count}) {
            out.push_back(copy(warning));
        }
    }
    return out;
}

Result<std::string> RenderScene::semantic_report(const SemanticReportOptions& options) const {
    const auto hrtf_sources = texts(options.capabilities.hrtf_sources);
    const MradmSceneReportOptions report{text(options.renderer),
                                         text(options.policy_path),
                                         options.capabilities.supports_channel_lock ? 1U : 0U,
                                         options.capabilities.supports_object_divergence ? 1U : 0U,
                                         options.capabilities.supports_diffuse ? 1U : 0U,
                                         hrtf_sources.data(),
                                         hrtf_sources.size(),
                                         text(options.renderer_effective_json)};
    std::array<uint8_t, 1024> message{};
    MradmAdmText output{};
    if (const auto code = mradm_scene_report(handle_->raw, &report, &output, message.data(), message.size());
        code != 0) {
        return make_error(static_cast<ErrorCode>(code), message_text(message), {});
    }
    return copy(output);
}

Result<std::string> RenderScene::policy_template() const {
    std::array<uint8_t, 1024> message{};
    MradmAdmText output{};
    if (const auto code = mradm_scene_template(handle_->raw, &output, message.data(), message.size()); code != 0) {
        return make_error(static_cast<ErrorCode>(code), message_text(message), {});
    }
    return copy(output);
}

Result<std::string> read_policy_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return make_error(ErrorCode::io_error, "semantic policy I/O error: cannot open file", path.string());
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

Result<void> write_policy_json(const std::filesystem::path& path, std::string_view json, std::string_view what) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return make_error(ErrorCode::io_error,
                          "semantic policy I/O error: cannot open " + std::string{what} + " for writing",
                          path.string());
    }
    out << json << '\n';
    if (!out) {
        return make_error(
            ErrorCode::io_error, "semantic policy I/O error: failed to write " + std::string{what}, path.string());
    }
    return {};
}

namespace {

InputChannelDefinition channel(const MradmInputChannelRecord& record) {
    return {copy(record.token), copy(record.speaker_label), record.azimuth, record.elevation, record.is_lfe != 0};
}

std::vector<InputChannelDefinition> channels(const MradmInputChannelRecord* records, size_t count) {
    std::vector<InputChannelDefinition> out;
    out.reserve(count);
    for (const auto& record : std::span{records, count}) {
        out.push_back(channel(record));
    }
    return out;
}

void check_table(int32_t code, const std::array<uint8_t, 1024>& message) {
    if (code != 0) {
        throw std::runtime_error(std::string{"input layout table: "} + reinterpret_cast<const char*>(message.data()));
    }
}

} // namespace

Result<AdmScene> import_scene(const std::string& path) {
    try {
        return RenderScene::import(path, nullptr).map([](const RenderScene& scene) { return scene.scene(); });
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options) {
    return RenderScene::import(path, &options).map([](const RenderScene& scene) { return scene.scene(); });
}

const std::vector<InputLayoutDefinition>& input_layouts() {
    static const std::vector<InputLayoutDefinition> layouts = [] {
        const MradmInputLayoutRecord* records = nullptr;
        size_t count = 0;
        std::array<uint8_t, 1024> message{};
        check_table(mradm_scene_input_layouts(&records, &count, message.data(), message.size()), message);
        std::vector<InputLayoutDefinition> out;
        out.reserve(count);
        for (const auto& record : std::span{records, count}) {
            out.push_back({copy(record.id),
                           copy(record.internal_id),
                           copy(record.display_name),
                           record.wave_channel_mask,
                           channels(record.channels, record.channels_len),
                           channels(record.wave_mask_channels, record.wave_mask_channels_len)});
        }
        return out;
    }();
    return layouts;
}

const std::vector<InputChannelDefinition>& input_channel_catalog() {
    static const std::vector<InputChannelDefinition> catalog = [] {
        const MradmInputChannelRecord* records = nullptr;
        size_t count = 0;
        std::array<uint8_t, 1024> message{};
        check_table(mradm_scene_channel_catalog(&records, &count, message.data(), message.size()), message);
        return channels(records, count);
    }();
    return catalog;
}

Result<std::vector<InputChannelDefinition>> resolve_input_channel_labels(const std::vector<std::string>& labels) {
    const auto raw = texts(labels);
    std::vector<uint32_t> indices(labels.size());
    std::array<uint8_t, 1024> message{};
    const auto code = mradm_scene_resolve_labels(
        raw.data(), raw.size(), indices.data(), indices.size(), message.data(), message.size());
    if (code != 0) {
        return tl::unexpected{scene_error(code, message, 0U, {})};
    }
    const auto& catalog = input_channel_catalog();
    std::vector<InputChannelDefinition> out;
    out.reserve(indices.size());
    std::ranges::transform(indices, std::back_inserter(out), [&catalog](auto index) { return catalog.at(index); });
    return out;
}

// Raw AXML for display and export. The CHNA records are still validated so a file the importer
// rejects is rejected here too.
Result<std::string> get_axml(const std::string& path) {
    try {
        auto chunks = audio::read_wav_adm_chunks(path);
        if (!chunks) {
            auto error = chunks.error();
            error.context = "input=" + path;
            return tl::unexpected{std::move(error)};
        }
        if (std::ranges::any_of(chunks->chna, [](const auto& record) { return record.track_index == 0U; })) {
            return make_error(ErrorCode::io_error, "CHNA trackIndex 必须从 1 开始", "input=" + path);
        }
        if (chunks->axml.empty()) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }
        return std::move(chunks->axml);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("读取 AXML 失败：") + e.what(), "input=" + path);
    }
}

// Only the axml payload is replaced (mradm-wav); every other chunk, PCM included, is copied byte for
// byte through a temporary file, so a failure never corrupts the destination or the source.
Result<void> RenderScene::write(const std::string& dst) const {
    auto xml = get_axml(path_);
    if (!xml) {
        return tl::unexpected{xml.error()};
    }
    const auto locator = "input=" + path_ + " output=" + dst;
    std::array<uint8_t, 1024> message{};
    MradmAdmText output{};
    if (const auto code = mradm_scene_export_axml(handle_->raw,
                                                  reinterpret_cast<const uint8_t*>(xml->data()),
                                                  xml->size(),
                                                  &output,
                                                  message.data(),
                                                  message.size());
        code != 0) {
        return make_error(static_cast<ErrorCode>(code), message_text(message), locator);
    }
    auto written = audio::replace_wav_axml(path_, dst, copy(output));
    if (!written) {
        auto error = written.error();
        error.context = locator;
        return tl::unexpected{std::move(error)};
    }
    return {};
}

} // namespace mradm::io
