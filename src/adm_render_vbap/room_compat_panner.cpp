#include "room_compat_panner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "room_222.h"

namespace mradm::room_compat {

namespace {

struct Node {
    double x{};
    std::size_t channel{};
};

struct Row {
    double y{};
    std::vector<Node> nodes;
};

// The compatibility profile uses an integer room grid for static Cartesian gains. The
// same geometry accounts for 7.1.4 and 9.1.6 training and held-out objects.
constexpr double k_horizontal_steps = 155.0;
constexpr double k_upper_extent = 80.0;
constexpr double k_wide_row_y = 105.0;
constexpr double k_vertical_steps = 75.0;

[[nodiscard]] std::pair<double, double> pair_weights(double fraction) noexcept {
    const double angle = std::clamp(fraction, 0.0, 1.0) * std::numbers::pi_v<double> * 0.5;
    return {std::cos(angle), std::sin(angle)};
}

[[nodiscard]] std::vector<double> pan_row(double x, const Row& row, std::size_t channels) {
    std::vector<double> gains(channels, 0.0);
    const auto& nodes = row.nodes;
    if (x <= nodes.front().x) {
        gains[nodes.front().channel] = 1.0;
        return gains;
    }
    if (x >= nodes.back().x) {
        gains[nodes.back().channel] = 1.0;
        return gains;
    }
    const auto upper =
        std::find_if(std::next(nodes.begin()), nodes.end(), [x](const Node& node) { return x <= node.x; });
    if (upper != nodes.end()) {
        const auto lower = std::prev(upper);
        const auto [left, right] = pair_weights((x - lower->x) / (upper->x - lower->x));
        gains[lower->channel] = left;
        gains[upper->channel] = right;
    }
    return gains;
}

[[nodiscard]] std::vector<double> pan_layer(double x, double y, const std::vector<Row>& rows, std::size_t channels) {
    if (y <= rows.front().y) {
        return pan_row(x, rows.front(), channels);
    }
    if (y >= rows.back().y) {
        return pan_row(x, rows.back(), channels);
    }
    const auto upper = std::find_if(std::next(rows.begin()), rows.end(), [y](const Row& row) { return y <= row.y; });
    if (upper != rows.end()) {
        const auto lower = std::prev(upper);
        auto low = pan_row(x, *lower, channels);
        const auto high = pan_row(x, *upper, channels);
        const auto [a, b] = pair_weights((y - lower->y) / (upper->y - lower->y));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            low[channel] = (a * low[channel]) + (b * high[channel]);
        }
        return low;
    }
    std::vector<double> silent(channels, 0.0); // unreachable with ordered rows
    return silent;
}

[[nodiscard]] std::vector<Row> middle_rows(bool nine_one_six) {
    std::vector<Row> rows{
        {-k_horizontal_steps, {{-k_horizontal_steps, 6}, {k_horizontal_steps, 7}}},
        {0.0, {{-k_horizontal_steps, 4}, {k_horizontal_steps, 5}}},
    };
    if (nine_one_six) {
        rows.push_back({k_wide_row_y, {{-k_horizontal_steps, 8}, {k_horizontal_steps, 9}}});
    }
    rows.push_back({k_horizontal_steps, {{-k_horizontal_steps, 0}, {0.0, 2}, {k_horizontal_steps, 1}}});
    return rows;
}

[[nodiscard]] std::vector<Row> upper_rows(bool nine_one_six) {
    if (!nine_one_six) {
        return {
            {-k_upper_extent, {{-k_upper_extent, 10}, {k_upper_extent, 11}}},
            {k_upper_extent, {{-k_upper_extent, 8}, {k_upper_extent, 9}}},
        };
    }
    return {
        {-k_upper_extent, {{-k_upper_extent, 14}, {k_upper_extent, 15}}},
        {0.0, {{-k_upper_extent, 12}, {k_upper_extent, 13}}},
        {k_upper_extent, {{-k_upper_extent, 10}, {k_upper_extent, 11}}},
    };
}

} // namespace

Result<std::vector<float>> point_gains(const SceneBlockPosition& position, float gain, std::string_view layout_id) {
    if (is_room_222(layout_id)) {
        auto values = room_222_gains(position);
        if (!values || !std::isfinite(gain)) {
            return values ? make_error(ErrorCode::unsupported, "room-222 requires finite gain")
                          : tl::unexpected{values.error()};
        }
        std::vector<float> result(values->begin(), values->end());
        std::ranges::transform(result, result.begin(), [gain](float value) { return value * gain; });
        return result;
    }
    const bool seven_one_four = layout_id == "4+7+0" || layout_id == "7.1.4";
    const bool nine_one_six = layout_id == "9.1.6";
    if (!seven_one_four && !nine_one_six) {
        return make_error(ErrorCode::unsupported, "room-compat supports only 7.1.4 and 9.1.6");
    }
    if (!position.cartesian || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        !std::isfinite(gain) || std::fabs(position.x) > 1.0F || std::fabs(position.y) > 1.0F || position.z < 0.0F ||
        position.z > 1.0F) {
        return make_error(ErrorCode::unsupported, "room-compat requires finite Cartesian XYZ within the ADM room");
    }

    const std::size_t channels = nine_one_six ? 16U : 12U;
    // The incoming ADM coordinates have float32 storage. Rounding horizontal
    // X before panning and negated Y before changing back to the ADM axis
    // reproduces the observed half-step behavior, including +/-0.5.
    const double x = std::floor((static_cast<double>(position.x) * k_horizontal_steps) + 0.5);
    const double y = -std::floor((-static_cast<double>(position.y) * k_horizontal_steps) + 0.5);
    const double vertical_code = std::floor((static_cast<double>(position.z) * k_vertical_steps) + 0.5);
    const double vertical_fraction = std::clamp(vertical_code / k_vertical_steps, 0.0, 1.0);
    const auto [middle_weight, upper_weight] = pair_weights(vertical_fraction);
    const auto middle = pan_layer(x, y, middle_rows(nine_one_six), channels);
    const auto upper = pan_layer(x, y, upper_rows(nine_one_six), channels);

    std::vector<float> result(channels, 0.0F);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        result[channel] = static_cast<float>((middle_weight * middle[channel] + upper_weight * upper[channel]) * gain);
    }
    return result;
}

} // namespace mradm::room_compat
