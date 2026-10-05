// Frozen from 1972817 for migration tests only.
#include "layout_222.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <span>
#include <utility>

namespace mradm::triple_balance_legacy {
namespace {
struct Row {
    double y{};
    std::vector<Room222Node> nodes;
};
struct Layer {
    double z{};
    std::vector<Row> rows;
};

const std::vector<Layer>& geometry() {
    // The reference room has narrower ceiling rows than the ear-level rows.
    // Keep that coordinate convention; these added nodes are our own topology.
    constexpr float a = 80.0F / 155.0F;
    constexpr float wide_y = 105.0F / 155.0F;
    static const std::vector<Layer> layers{
        {-1, {{a, {{22, "B+045", -a, a, -1, 2, 1}, {21, "B+000", 0, a, -1, -1, 0}, {23, "B-045", a, a, -1, 2, -1}}}}},
        {0,
         {{-1, {{4, "M+135", -1, -1, 0, 2, 1}, {8, "M+180", 0, -1, 0, -1, 0}, {5, "M-135", 1, -1, 0, 2, -1}}},
          {0, {{10, "M+090", -1, 0, 0, 1, 1}, {11, "M-090", 1, 0, 0, 1, -1}}},
          {wide_y, {{0, "M+060", -1, wide_y, 0, 1, 1}, {1, "M-060", 1, wide_y, 0, 1, -1}}},
          {1, {{6, "M+030", -1, 1, 0, 0, 1}, {2, "M+000", 0, 1, 0, -1, 0}, {7, "M-030", 1, 1, 0, 0, -1}}}}},
        {1,
         {{-a, {{16, "U+135", -a, -a, 1, 0, 1}, {20, "U+180", 0, -a, 1, -1, 0}, {17, "U-135", a, -a, 1, 0, -1}}},
          {0, {{18, "U+090", -a, 0, 1, 0, 1}, {15, "T+000", 0, 0, 1, -1, 0}, {19, "U-090", a, 0, 1, 0, -1}}},
          {a, {{12, "U+045", -a, a, 1, 3, 1}, {14, "U+000", 0, a, 1, -1, 0}, {13, "U-045", a, a, 1, 3, -1}}}}}};
    return layers;
}

// Average the squared equal-power weights over an interval. Outside the
// outer knots the position projects to the nearest row/node. A single knot
// therefore represents the explicitly documented lower front-row projection.
std::vector<double> power_weights(std::span<const double> knots, double lo, double hi) {
    std::vector<double> weights(knots.size(), 0);
    if (knots.size() == 1) {
        weights.front() = 1;
        return weights;
    }
    if (hi - lo < 1e-10) {
        const double x = (lo + hi) * .5;
        if (x <= knots.front()) {
            weights.front() = 1;
        } else if (x >= knots.back()) {
            weights.back() = 1;
        } else {
            const auto upper = std::ranges::lower_bound(knots, x);
            const auto i = static_cast<std::size_t>(std::distance(knots.begin(), upper));
            const double angle = (x - knots[i - 1]) / (knots[i] - knots[i - 1]) * std::numbers::pi / 2;
            weights[i - 1] = std::pow(std::cos(angle), 2);
            weights[i] = std::pow(std::sin(angle), 2);
        }
        return weights;
    }
    weights.front() += std::max(0.0, std::min(hi, knots.front()) - lo);
    weights.back() += std::max(0.0, hi - std::max(lo, knots.back()));
    for (std::size_t i = 1; i < knots.size(); ++i) {
        const double left = std::max(lo, knots[i - 1]);
        const double right = std::min(hi, knots[i]);
        if (left >= right) {
            continue;
        }
        const double span = knots[i] - knots[i - 1];
        const auto integral_cos = [&](double x) {
            const double t = x - knots[i - 1];
            return (t * .5) + (span * std::sin(std::numbers::pi * t / span) / (2 * std::numbers::pi));
        };
        const double before = integral_cos(right) - integral_cos(left);
        weights[i - 1] += before;
        weights[i] += right - left - before;
    }
    std::ranges::transform(
        weights, weights.begin(), [lo, hi](double value) { return std::max(0.0, value / (hi - lo)); });
    return weights;
}

std::pair<double, double> interval(float center, float size) {
    return {std::max(-1.0, static_cast<double>(center) - size), std::min(1.0, static_cast<double>(center) + size)};
}
} // namespace

bool is_room_222(std::string_view layout) noexcept {
    return layout == "9+10+3" || layout == "22.2";
}

const std::vector<Room222Node>& room_222_nodes() {
    static const auto nodes = [] {
        std::vector<Room222Node> result;
        for (const auto& layer : geometry()) {
            for (const auto& row : layer.rows) {
                result.insert(result.end(), row.nodes.begin(), row.nodes.end());
            }
        }
        return result;
    }();
    return nodes;
}

Result<Room222Gains> room_222_gains(const SceneBlockPosition& position, float size) {
    if (!position.cartesian || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
        std::fabs(position.x) > 1 || std::fabs(position.y) > 1 || std::fabs(position.z) > 1 || !std::isfinite(size) ||
        size < 0 || size > 1) {
        return make_error(ErrorCode::unsupported, "room-222 requires Cartesian XYZ in [-1,1] and equal size in [0,1]");
    }
    const auto [xlo, xhi] = interval(position.x, size);
    const auto [ylo, yhi] = interval(position.y, size);
    const auto [zlo, zhi] = interval(position.z, size);
    const auto& layers = geometry();
    std::vector<double> heights;
    heights.reserve(layers.size());
    std::ranges::transform(layers, std::back_inserter(heights), [](const auto& layer) { return layer.z; });
    const auto vertical = power_weights(heights, zlo, zhi);
    std::array<double, k_room_222_channels> power{};
    for (std::size_t i = 0; i < layers.size(); ++i) {
        std::vector<double> depths;
        depths.reserve(layers[i].rows.size());
        std::ranges::transform(layers[i].rows, std::back_inserter(depths), [](const auto& row) { return row.y; });
        const auto rows = power_weights(depths, ylo, yhi);
        for (std::size_t j = 0; j < layers[i].rows.size(); ++j) {
            const auto& nodes = layers[i].rows[j].nodes;
            std::vector<double> widths;
            widths.reserve(nodes.size());
            std::ranges::transform(nodes, std::back_inserter(widths), [](const auto& node) { return node.x; });
            const auto horizontal = power_weights(widths, xlo, xhi);
            for (std::size_t k = 0; k < nodes.size(); ++k) {
                power.at(nodes[k].channel) += vertical[i] * rows[j] * horizontal[k];
            }
        }
    }
    Room222Gains gains{};
    std::ranges::transform(power, gains.begin(), [](double value) { return static_cast<float>(std::sqrt(value)); });
    return gains;
}

Room222Mix room_222_mix(const Room222Gains& spatial, float size) {
    const float phase = std::clamp(size / .2F, 0.0F, 1.0F) * std::numbers::pi_v<float> / 2;
    const float sine = std::sin(phase);
    const float cosine = std::cos(phase);
    Room222Mix result{size >= .2F ? 0.0F : cosine * cosine, {}};
    float front = 0;
    float rest = 0;
    for (std::size_t i = 0; i < spatial.size(); ++i) {
        result.spread.at(i) = spatial.at(i) * (sine * sine);
        (i == 2 || i == 6 || i == 7 ? front : rest) += result.spread.at(i) * result.spread.at(i);
    }
    if (front + rest > 1e-6F) {
        const float attenuation = std::pow(10.0F, size * -1.2F / 20);
        const float square = attenuation * attenuation;
        const float common = std::sqrt(((front + rest) * square) / (front + rest * square));
        for (std::size_t i = 0; i < spatial.size(); ++i) {
            result.spread.at(i) *= i == 2 || i == 6 || i == 7 ? common : attenuation * common;
        }
    }
    return result;
}
} // namespace mradm::triple_balance_legacy
