#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "adm/direct_speakers_matrix.h"
#include "adm/screen.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void terminal_nul_test() {
    constexpr std::string_view matrix_json = R"({"schema":"mradm.direct-speakers-matrix.v1","output_layout":"5.1",
        "routes":[{"source_label":"C","targets":[{"label":"L","weight":1},{"label":"R","weight":3}]}]})";
    constexpr std::string_view screen_json = R"({"schema":"mradm.screen-config","version":1,
        "reference":{"coordinates":"polar","centre":[0,0,1],"width":58,"aspect_ratio":1.78}})";
    for (const auto& suffix : {std::string(1, '\0'), std::string{" \0trailing", 10}}) {
        const auto matrix = mradm::parse_direct_speakers_matrix(std::string{matrix_json} + suffix);
        require(matrix && matrix->routes.size() == 1 && matrix->routes[0].targets.size() == 2,
                "NUL-terminated matrix retains routes");
        require(matrix->routes[0].targets[0].gain == 0.5F, "NUL-terminated matrix retains gain");
        const auto screens = mradm::parse_screen_config_json(std::string{screen_json} + suffix);
        require(screens && screens->reference && screens->reference->width == 58.0,
                "NUL-terminated screen retains geometry");
    }
}

void matrix_error_bytes_test() {
    const std::string key{"x\0y", 3};
    const auto unknown = mradm::parse_direct_speakers_matrix(R"({"x\u0000y":1})", "matrix-source");
    require(!unknown && unknown.error().code == mradm::ErrorCode::invalid_argument,
            "unknown matrix key is invalid_argument");
    require(unknown.error().message == "invalid DirectSpeakers matrix: unknown top-level field '" + key + "'",
            "unknown matrix key error preserves embedded NUL and suffix");
    require(unknown.error().context == "matrix-source", "matrix error retains source context");
    const auto schema = mradm::parse_direct_speakers_matrix(R"({"schema":"x\u0000y"})");
    require(!schema && schema.error().message == "invalid DirectSpeakers matrix: unsupported schema '" + key + "'",
            "matrix schema error preserves embedded NUL and suffix");
}

void screen_overflow_test() {
    for (const auto length : {1024U, 10000U}) {
        const std::string number(length, '9');
        const auto screens =
            mradm::parse_screen_config_json(R"({"schema":"mradm.screen-config","version":1,"extra":)" + number + "}");
        require(!screens && screens.error().code == mradm::ErrorCode::invalid_argument,
                "screen numeric overflow is invalid_argument");
        require(screens.error().message ==
                    "无效的屏幕配置: [json.exception.out_of_range.406] number overflow parsing '" + number + "'",
                "screen overflow error retains the complete number");
        require(screens.error().context.empty(), "screen parse error has no source context");
    }
    const auto oversized = mradm::parse_screen_config_json(std::string((1024U * 1024U) + 1U, ' '));
    require(!oversized && oversized.error().message == "屏幕配置超过 1 MiB", "screen byte limit is unchanged");
}
} // namespace

int main() {
    try {
        terminal_nul_test();
        matrix_error_bytes_test();
        screen_overflow_test();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
