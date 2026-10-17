#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "adm/audio_io.h"
#include "adm/io.h"

#include "metadata.h"
#include "scene_ffi.h"
#include "scene_import_internal.h"
#include "wav_backend.h"

namespace mradm::io {

// The import itself is Rust (mradm-scene, PCM consistency 4.3): WAVE format, the CHNA channel policy,
// the ADM projection and channel-bed synthesis. This adapter builds the public scene from its view.
namespace {

using SceneHandle = std::unique_ptr<MradmSceneHandle, decltype(&mradm_scene_destroy)>;

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
    for (const auto& value : values) {
        out.push_back(text(value));
    }
    return out;
}

Result<AdmScene> import(const std::string& path, const SceneImportOptions* options, const char* checkpoint) {
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
    SceneHandle handle{raw, mradm_scene_destroy};
    if (code != 0) {
        return tl::unexpected{scene_error(code, message, context, path)};
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    if (checkpoint != nullptr) {
        // Diagnostic only; a failed write is detected by the collector's checkpoint inventory.
        (void) mradm_scene_checkpoint(handle.get(), text(checkpoint), message.data(), message.size());
    }
#else
    (void) checkpoint;
#endif
    MradmSceneImportInfo info{};
    MradmAdmView view{};
    if (const auto view_code = mradm_scene_view(handle.get(), &info, &view, message.data(), message.size());
        view_code != 0) {
        return tl::unexpected{scene_error(view_code, message, 1U, path)};
    }
    const bool channel_bed = info.source_kind == 1U;
    auto scene = metadata::scene_from_view(view, !channel_bed);
    if (!scene) {
        auto error = scene.error();
        error.context = "input=" + path;
        return tl::unexpected{std::move(error)};
    }
    auto& scene_info = scene->info;
    scene_info.sample_rate = info.sample_rate;
    scene_info.num_channels = static_cast<uint16_t>(info.channels);
    scene_info.num_frames = info.frames;
    scene_info.file_path = path;
    scene_info.source_kind = channel_bed ? SceneSourceKind::channel_bed : SceneSourceKind::adm;
    scene_info.input_layout = copy(info.input_layout);
    for (const auto& label : std::span{info.input_channel_labels, info.input_channel_labels_len}) {
        scene_info.input_channel_labels.push_back(copy(label));
    }
    return scene;
}

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
        return import(path, nullptr, nullptr);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options) {
    return import(path, &options, nullptr);
}

Result<AdmScene> import_scene_for_render(const std::string& path, const SceneImportOptions& options) {
    return import(path, &options, "05-import");
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
    for (const auto index : indices) {
        out.push_back(catalog.at(index));
    }
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
        for (const auto& record : chunks->chna) {
            if (record.track_index == 0U) {
                return make_error(ErrorCode::io_error, "CHNA trackIndex 必须从 1 开始", "input=" + path);
            }
        }
        if (chunks->axml.empty()) {
            return make_error(ErrorCode::io_error, "axml chunk 缺失或为空", "input=" + path);
        }
        return std::move(chunks->axml);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("读取 AXML 失败：") + e.what(), "input=" + path);
    }
}

namespace {

// ---- chunk-level BWF rewrite (bit-exact PCM) ----
// mradm-wav 只替换 axml chunk 的 payload，其余 chunk（data / chna / fmt / bext / JUNK / 未知）
// 的原始字节逐块复制，绝不解码 / 重编码 PCM —— 这是音频真正 bit-exact 的唯一方式。

// Rewrite src_path → dst_path through a temporary file so a failure never corrupts the destination
// (or the source when dst == src).
Result<void>
rewrite_bwf_replacing_axml(const std::string& src_path, const std::string& dst_path, const std::string& new_axml) {
    auto written = audio::replace_wav_axml(src_path, dst_path, new_axml);
    if (!written) {
        auto error = written.error();
        error.context = "input=" + src_path + " output=" + dst_path;
        return tl::unexpected{std::move(error)};
    }
    return {};
}

} // namespace

Result<void> write_scene(const std::string& src_path,
                         const AdmScene& original,
                         const AdmScene& effective,
                         const std::string& dst_path) {
    try {
        auto xml_res = get_axml(src_path);
        if (!xml_res) {
            return tl::unexpected{xml_res.error()};
        }
        auto patched = metadata::patch_axml(*xml_res, original, effective);
        if (!patched) {
            auto error = patched.error();
            error.context = "input=" + src_path + " output=" + dst_path;
            return tl::unexpected{std::move(error)};
        }
        return rewrite_bwf_replacing_axml(src_path, dst_path, *patched);

    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error,
                          std::string("写回 ADM 文件失败：") + e.what(),
                          "input=" + src_path + " output=" + dst_path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "写回 ADM 文件时发生未知异常", "input=" + src_path);
    }
}

} // namespace mradm::io
