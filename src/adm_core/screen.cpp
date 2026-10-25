#include "adm/screen.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <vector>

#include "config_ffi.h"

namespace mradm {
namespace {
constexpr std::size_t k_max_screen_json = std::size_t{1024} * 1024;
MradmRoomScreenGeometry convert(const ScreenGeometry& screen) {
    return {screen.cartesian ? 1U : 0U,
            0,
            screen.aspect_ratio,
            {screen.centre[0], screen.centre[1], screen.centre[2]},
            screen.width};
}
ScreenGeometry convert(const MradmRoomScreenGeometry& screen) {
    return {screen.cartesian != 0U,
            screen.aspect_ratio,
            {screen.centre[0], screen.centre[1], screen.centre[2]},
            screen.width};
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

Result<ScreenConfig> parse_screen_config_json(std::string_view source) {
    if (source.size() > k_max_screen_json) {
        return make_error(ErrorCode::invalid_argument, "屏幕配置超过 1 MiB");
    }
    MradmScreenConfigView view{};
    // Numeric overflow diagnostics contain the complete input token plus fixed text.
    std::string message(source.size() + 1024, '\0');
    auto message_len = message.size();
    const auto code = mradm_scene_screen_parse(
        reinterpret_cast<const uint8_t*>(source.data()), source.size(), &view, message.data(), &message_len);
    if (code != 0) {
        message.resize(message_len);
        return make_error(static_cast<ErrorCode>(code), std::move(message));
    }
    const ScreenConfig config{view.has_reference != 0U ? std::optional{convert(view.reference)} : std::nullopt,
                              view.has_reproduction != 0U ? std::optional{convert(view.reproduction)} : std::nullopt};
    if (auto valid = validate_screen_config(config); !valid) {
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

} // namespace mradm
