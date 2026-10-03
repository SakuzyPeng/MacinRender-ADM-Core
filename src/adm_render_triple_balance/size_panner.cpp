#include "size_panner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>

namespace mradm::triple_balance {
// Indices below are bounded by the fixed geometry/array extents. Keep scalar
// operation order visible because it is checked against float32 reference data.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index, readability-math-missing-parentheses)
namespace {
constexpr float k_room_max = 32767.0F / 32768.0F;
constexpr float k_pi_half = std::numbers::pi_v<float> * 0.5F;
constexpr float k_radius_step = 0.7F / 19.0F;
constexpr std::array<SizePosition, 11> k_speakers{{
    {0, 0, 0},
    {k_room_max, 0, 0},
    {0.5F, 0, 0},
    {0, 0.5F, 0},
    {k_room_max, 0.5F, 0},
    {0, k_room_max, 0},
    {k_room_max, k_room_max, 0},
    {0, 0, k_room_max},
    {k_room_max, 0, k_room_max},
    {0, k_room_max, k_room_max},
    {k_room_max, k_room_max, k_room_max},
}};
constexpr std::array<std::array<std::size_t, 3>, 5> k_rows{{{0, 2, 1}, {3, 4, 4}, {5, 6, 6}, {7, 8, 8}, {9, 10, 10}}};
constexpr std::array<std::size_t, 5> k_row_counts{3, 2, 2, 2, 2};

// Low-order log2/exp2 approximations are part of the observed spatial rule.
// Replacing these with pow() changes the field, especially at small extents.
float log2_approx(float value) noexcept {
    int exponent = 0;
    const float mantissa = std::frexp(value, &exponent);
    return ((mantissa * 4.079345703125F - 2.693115234375F) + (mantissa * mantissa) * -1.38623046875F) +
           static_cast<float>(exponent);
}

float exp2_approx(float value) noexcept {
    const float exponent = std::floor(value);
    const float fraction = value - exponent;
    const float square = fraction * fraction;
    return std::ldexp((fraction * square) * 0.05889892578125F +
                          (square * 0.25482177734375F + (fraction * 0.686279296875F + 1.0F)),
                      static_cast<int>(exponent));
}

float power(float value, float exponent) noexcept {
    return value > 0 ? exp2_approx(log2_approx(value) * exponent) : 0.0F;
}

float repeated_step(float step, unsigned count) noexcept {
    float result = 0;
    for (unsigned i = 0; i < count; ++i) {
        result += step;
    }
    return result;
}

void normalize(SizeGains& values) noexcept {
    float sum = 0;
    for (const auto value : values) {
        // cppcheck-suppress useStlAlgorithm
        sum += value * value;
    }
    const float norm = std::max(std::sqrt(sum), 1e-5F);
    for (auto& value : values) {
        // cppcheck-suppress useStlAlgorithm
        value /= norm;
    }
}

float shape_exponent(float radius) noexcept {
    return radius <= 0.125F ? 6.0F : radius * -6.956521987915039F + 6.869565010070801F;
}

float radius_from_size(float size) noexcept {
    constexpr std::array<float, 5> sizes{0, 0.2F, 0.5F, 0.75F, 1};
    constexpr std::array<float, 5> radii{0, 0.075F, 0.25F, 0.45F, 0.7F};
    for (std::size_t i = 1; i < sizes.size(); ++i) {
        // cppcheck-suppress useStlAlgorithm
        if (size <= sizes[i]) {
            return radii[i - 1] + (size - sizes[i - 1]) / (sizes[i] - sizes[i - 1]) * (radii[i] - radii[i - 1]);
        }
    }
    return 1.0F;
}

float kernel(float point, float center, float radius, unsigned axis) noexcept {
    if (radius < 2.5e-5F) {
        return 0;
    }
    const float delta = (point - center) * 0.25F;
    if (std::fabs(delta) > radius) {
        return 0;
    }
    const float ratio = delta / radius;
    const float square = ratio * ratio;
    float result = exp2_approx((square * square * -0.6328125F) * 26.575424194335938F);
    if (axis == 2) {
        result *= std::cos((point * 0.34147748351097107F) * 4.0F);
    }
    return result;
}

SizeGains axis_basis(unsigned axis, float position) noexcept {
    SizeGains result{};
    if (axis == 0) {
        for (std::size_t row = 0; row < k_rows.size(); ++row) {
            const auto& nodes = k_rows[row];
            const auto count = k_row_counts[row];
            if (position <= k_speakers[nodes[0]].x) {
                result[nodes[0]] = 1;
            } else if (position >= k_speakers[nodes[count - 1]].x) {
                result[nodes[count - 1]] = 1;
            } else {
                for (std::size_t i = 1; i < count; ++i) {
                    const auto left = nodes[i - 1];
                    const auto right = nodes[i];
                    if (position <= k_speakers[right].x) {
                        const float fraction =
                            (position - k_speakers[left].x) / (k_speakers[right].x - k_speakers[left].x);
                        result[left] = std::cos(fraction * k_pi_half);
                        result[right] = std::sin(fraction * k_pi_half);
                        break;
                    }
                }
            }
        }
    } else if (axis == 1) {
        for (std::size_t layer = 0; layer < 2; ++layer) {
            const std::size_t start = layer == 0 ? 0 : 3;
            const std::size_t count = layer == 0 ? 3 : 2;
            std::array<float, 3> row_gains{};
            if (position <= 0) {
                row_gains[0] = 1;
            } else if (position >= k_room_max) {
                row_gains[count - 1] = 1;
            } else {
                for (std::size_t i = 1; i < count; ++i) {
                    const float low = k_speakers[k_rows[start + i - 1][0]].y;
                    const float high = k_speakers[k_rows[start + i][0]].y;
                    if (position <= high) {
                        const float fraction = (position - low) / (high - low);
                        row_gains[i - 1] = std::cos(fraction * k_pi_half);
                        row_gains[i] = std::sin(fraction * k_pi_half);
                        break;
                    }
                }
            }
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t j = 0; j < k_row_counts[start + i]; ++j) {
                    result[k_rows[start + i][j]] = row_gains[i];
                }
            }
        }
    } else {
        const float lower = position >= k_room_max ? 0 : std::cos(position * k_pi_half);
        const float upper = position >= k_room_max ? 1 : std::sin(position * k_pi_half);
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = i < 7 ? lower : upper;
        }
    }
    return result;
}

// Generate a quadrature value from the room geometry. These are not measured
// object-gain tables: every term is an axis basis times the same analytic kernel.
SizeGains quadrature(unsigned axis, unsigned position_index, unsigned radius_index) noexcept {
    const auto count = axis == 2 ? 8U : 20U;
    const float step = axis == 2 ? 1.0F / 7.0F : 1.0F / 19.0F;
    const float center = repeated_step(axis == 2 ? 1.0F / 3.0F : 1.0F / 34.0F, position_index);
    const float radius = repeated_step(k_radius_step, radius_index);
    const float exponent = shape_exponent(radius);
    SizeGains result{};
    float position = 0;
    for (unsigned sample = 0; sample < count; ++sample, position += step) {
        const auto basis = axis_basis(axis, position);
        const float weight = kernel(position, center, radius, axis);
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] += power(basis[i] * weight, exponent);
        }
    }
    return result;
}

SizeGains axis_extent(unsigned axis, float coordinate, float radius) noexcept {
    const auto steps = axis == 2 ? 3U : 34U;
    const float coordinate_step = 1.0F / static_cast<float>(steps);
    const auto a = std::min(static_cast<unsigned>(std::floor(coordinate * static_cast<float>(steps))), steps);
    const auto b = std::min(a + 1, steps);
    const float x =
        a == b ? 0 : std::max((coordinate - coordinate_step * static_cast<float>(a)) / coordinate_step, 0.0F);
    const auto c =
        std::min(static_cast<unsigned>(std::floor((radius * (1.0F / 1.4F) + radius * (1.0F / 1.4F)) * 19.0F)), 19U);
    const auto d = std::min(c + 1, 19U);
    const float y = c == d ? 0 : std::max((radius - k_radius_step * static_cast<float>(c)) / k_radius_step, 0.0F);
    std::array<float, 4> weights{
        1 - std::max(x, y), 1 - std::max(1 - x, y), 1 - std::max(x, 1 - y), 1 - std::max(1 - x, 1 - y)};
    const float sum = weights[0] + weights[1] + weights[2] + weights[3];
    const std::array<SizeGains, 4> corners{
        quadrature(axis, a, c), quadrature(axis, b, c), quadrature(axis, a, d), quadrature(axis, b, d)};
    SizeGains result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        for (std::size_t corner = 0; corner < 4; ++corner) {
            result[i] += (weights[corner] / sum) * corners[corner][i];
        }
    }
    return result;
}
} // namespace

Result<QuantizedSizeParameters> quantize_size_parameters(SizePosition position, float size) {
    const std::array<float, 4> values{position.x, position.y, position.z, size};
    if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; })) {
        return make_error(ErrorCode::unsupported, "triple-balance size requires finite internal XYZ and size in [0,1]");
    }
    const auto quantize = [](float value) {
        return std::min(static_cast<int32_t>(std::floor(value * 32768.0F + 0.5F)), 32767);
    };
    return QuantizedSizeParameters{{quantize(position.x), quantize(position.y), quantize(position.z)}, quantize(size)};
}

Result<SizeGains> raw_size_gains(const QuantizedSizeParameters& parameters) {
    if (parameters.size < 0 || parameters.size > 32767 ||
        !std::ranges::all_of(parameters.xyz, [](int32_t value) { return value >= 0 && value <= 32767; })) {
        return make_error(ErrorCode::unsupported, "triple-balance size requires valid quantized room parameters");
    }
    const float x = static_cast<float>(parameters.xyz[0]) / 32768.0F;
    const float y = static_cast<float>(parameters.xyz[1]) / 32768.0F;
    const float z = static_cast<float>(parameters.xyz[2]) / 32768.0F;
    const float radius = radius_from_size(static_cast<float>(parameters.size) / 32768.0F);
    if (radius < 2.5e-5F) {
        const auto px = axis_basis(0, x);
        const auto py = axis_basis(1, y);
        const auto pz = axis_basis(2, z);
        SizeGains result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            result[i] = px[i] * py[i] * pz[i];
        }
        return result;
    }
    const float exponent = shape_exponent(radius);
    const auto ax = axis_extent(0, x, radius);
    const auto ay = axis_extent(1, y, radius);
    const auto az = axis_extent(2, z, radius);
    const std::array<float, 5> wall{kernel(0, x, radius, 0),
                                    kernel(1, x, radius, 0),
                                    kernel(0, y, radius, 1),
                                    kernel(1, y, radius, 1),
                                    kernel(1, z, radius, 0) * 0.20345592498779297F};
    SizeGains volume{};
    SizeGains boundary{};
    for (std::size_t i = 0; i < volume.size(); ++i) {
        const auto& speaker = k_speakers[i];
        volume[i] = (ax[i] * ay[i]) * az[i];
        const float roof = speaker.z > 0.5F ? power(wall[4], exponent) : 0;
        const float left = speaker.x == 0 ? power(wall[0], exponent) : 0;
        const float right = speaker.x == k_room_max ? power(wall[1], exponent) : 0;
        const float front = speaker.y == 0 ? power(wall[2], exponent) : 0;
        const float rear = speaker.y == k_room_max ? power(wall[3], exponent) : 0;
        boundary[i] = ((roof * (ax[i] * ay[i]) + (ay[i] * az[i]) * left) + (ay[i] * az[i]) * right) +
                      (ax[i] * az[i]) * front + (ax[i] * az[i]) * rear;
    }
    normalize(volume);
    normalize(boundary);
    const float distance = std::min({x, 1 - x, y, 1 - y, 1 - z});
    float interior_weight = 1;
    if (distance * 0.25F <= radius || distance * 0.25F <= 0.05F) {
        interior_weight = (distance / (radius * 4)) * ((distance * 5) * (distance * 5));
    }
    SizeGains result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = power(((boundary[i] + interior_weight * volume[i]) * 0.5F) * 0.0625F, 1 / exponent);
    }
    normalize(result);
    return result;
}

SizeMixGains mix_size_gains(const SizeGains& raw, float effective_size) noexcept {
    const float phase = std::clamp(effective_size / 0.2F, 0.0F, 1.0F) * k_pi_half;
    const float sine = std::sin(phase);
    const float cosine = std::cos(phase);
    SizeMixGains result{effective_size >= 0.2F ? 0.0F : cosine * cosine, {}};
    float front = 0;
    float rest = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        result.spread[i] = raw[i] * (sine * sine);
        (i < 3 ? front : rest) += result.spread[i] * result.spread[i];
    }
    if (front + rest > 1e-6F) {
        const float attenuation = std::pow(10.0F, effective_size * -1.2F / 20.0F);
        const float square = attenuation * attenuation;
        const float common = std::sqrt(((front + rest) * square) / (front + rest * square));
        for (std::size_t i = 0; i < raw.size(); ++i) {
            result.spread[i] *= i < 3 ? common : attenuation * common;
        }
    }
    for (auto& gain : result.spread) {
        if (gain < 1e-6F) {
            // cppcheck-suppress useStlAlgorithm
            gain = 0;
        }
    }
    return result;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index, readability-math-missing-parentheses)
} // namespace mradm::triple_balance
