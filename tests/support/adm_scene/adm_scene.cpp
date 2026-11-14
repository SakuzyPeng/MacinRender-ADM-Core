#include "adm_scene.h"

#include <array>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "metadata.h"
#include "scene_handle.h"
#include "scene_import_internal.h"
#include "scene_test_ffi.h"

namespace mradm {

namespace {

std::string copy(MradmAdmText value) {
    return value.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(value.data), value.len};
}

} // namespace

Result<AdmScene> adm_scene(const detail::RenderSceneHandle& handle, const std::string& path) {
    std::array<uint8_t, 1024> message{};
    MradmSceneImportInfo info{};
    MradmAdmView view{};
    MradmSceneOverlay overlay{};
    auto code = mradm_scene_view(handle.raw, &info, &view, message.data(), message.size());
    if (code == 0) {
        code = mradm_scene_overlay(handle.raw, &overlay, message.data(), message.size());
    }
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), reinterpret_cast<const char*>(message.data()), "input=" + path);
    }
    const bool channel_bed = info.source_kind == 1U;
    auto scene = metadata::scene_from_view(view, !channel_bed, &overlay);
    if (!scene) {
        auto error = scene.error();
        error.context = "input=" + path;
        return mradm::unexpected{std::move(error)};
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

AdmScene adm_scene(const PreparedScene& scene) {
    auto result = adm_scene(*scene.handle);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

} // namespace mradm

namespace mradm::io {

namespace {

Result<AdmScene> imported(const std::string& path, const SceneImportOptions* options) {
    auto scene = RenderScene::import(path, options);
    if (!scene) {
        return mradm::unexpected{scene.error()};
    }
    return mradm::adm_scene(*scene->handle(), path);
}

} // namespace

Result<AdmScene> import_scene(const std::string& path) {
    try {
        return imported(path, nullptr);
    } catch (const std::exception& e) {
        return make_error(ErrorCode::io_error, std::string("解析 BW64/ADM 文件失败：") + e.what(), "input=" + path);
    } catch (...) {
        return make_error(ErrorCode::io_error, "解析 BW64/ADM 文件时发生未知异常", "input=" + path);
    }
}

Result<AdmScene> import_scene(const std::string& path, const SceneImportOptions& options) {
    return imported(path, &options);
}

AdmScene adm_scene(const RenderScene& scene) {
    auto result = mradm::adm_scene(*scene.handle());
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

} // namespace mradm::io
