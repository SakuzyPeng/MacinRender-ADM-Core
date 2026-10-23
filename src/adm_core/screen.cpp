#include "adm/screen.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "adm/scene.h"

#include "config_ffi.h"

namespace mradm {
namespace {
constexpr std::size_t k_max_screen_json = std::size_t{1024} * 1024;
MradmRoomScreenGeometry convert(const ScreenGeometry& screen) {
    MradmRoomScreenGeometry out{};
    out.cartesian = screen.cartesian ? 1U : 0U;
    out.aspect_ratio = screen.aspect_ratio;
    std::ranges::copy(screen.centre, std::begin(out.centre));
    out.width = screen.width;
    return out;
}
ScreenGeometry convert(const MradmRoomScreenGeometry& screen) {
    return {screen.cartesian != 0U,
            screen.aspect_ratio,
            {screen.centre[0], screen.centre[1], screen.centre[2]},
            screen.width};
}
bool contains_object(const AdmScene& scene, std::vector<std::string> pending, std::string_view wanted) {
    std::set<std::string> visited;
    while (!pending.empty()) {
        auto id = std::move(pending.back());
        pending.pop_back();
        if (id == wanted) {
            return true;
        }
        if (!visited.insert(id).second) {
            continue;
        }
        const auto object = std::ranges::find(scene.objects, id, &SceneObject::id);
        if (object == scene.objects.end()) {
            continue;
        }
        const auto& source = object->adm_source;
        if (source) {
            const auto& children = source->child_objects;
            pending.insert(pending.end(), children.begin(), children.end());
        }
    }
    return false;
}
} // namespace

Result<void> validate_screen_config(const ScreenConfig& config) {
    MradmRoomScreens screens{};
    screens.enabled = 1;
    const auto reference = config.reference.value_or(ScreenGeometry{});
    screens.reference = convert(reference);
    screens.reproduction = convert(config.reproduction.value_or(reference));
    std::array<char, 512> message{};
    const auto code = mradm_room_validate_screens(&screens, 1, message.data(), message.size());
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), message.data(), "screen");
    }
    return {};
}

// Parsed in Rust mradm-scene `screen` (PCM consistency 4.4); messages carry no input text.
Result<ScreenConfig> parse_screen_config_json(std::string_view source) {
    MradmScreenConfigView view{};
    std::array<uint8_t, 1024> message{};
    const auto code = mradm_scene_screen_parse(
        reinterpret_cast<const uint8_t*>(source.data()), source.size(), &view, message.data(), message.size());
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), reinterpret_cast<const char*>(message.data()));
    }
    ScreenConfig config;
    if (view.has_reference != 0U) {
        config.reference = convert(view.reference);
    }
    if (view.has_reproduction != 0U) {
        config.reproduction = convert(view.reproduction);
    }
    auto valid = validate_screen_config(config);
    if (!valid) {
        return tl::unexpected{valid.error()};
    }
    return config;
}

Result<ScreenConfig> load_screen_config(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return make_error(ErrorCode::io_error, "无法读取屏幕配置文件");
    }
    std::string text;
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        text.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        if (text.size() > k_max_screen_json) {
            return make_error(ErrorCode::invalid_argument, "屏幕配置超过 1 MiB");
        }
    }
    if (input.bad()) {
        return make_error(ErrorCode::io_error, "读取屏幕配置失败");
    }
    return parse_screen_config_json(text);
}

Result<ScreenGeometry>
resolve_reference_screen(const AdmScene& scene, std::string_view object_id, const ScreenConfig& config) {
    if (config.reference) {
        return *config.reference;
    }
    if (!config.reproduction) {
        return ScreenGeometry{};
    }
    std::optional<ScreenGeometry> selected;
    for (const auto& programme : scene.programmes) {
        bool member = false;
        for (const auto& id : programme.content_ids) {
            const auto content = std::ranges::find(scene.contents, id, &SceneContent::id);
            if (content != scene.contents.end() && contains_object(scene, content->object_ids, object_id)) {
                member = true;
                break;
            }
        }
        if (!member) {
            continue;
        }
        const auto screen = programme.reference_screen.value_or(ScreenGeometry{});
        if (selected && *selected != screen) {
            return make_error(ErrorCode::invalid_argument,
                              "对象属于制作屏幕不同的 Programme；请在屏幕配置中显式覆盖 reference");
        }
        selected = screen;
    }
    return selected.value_or(ScreenGeometry{});
}
} // namespace mradm
