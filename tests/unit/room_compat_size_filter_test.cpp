#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

#include "room_compat_size_filter.h"

namespace {

bool require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

} // namespace

int main() {
    std::array<std::vector<float>, mradm::room_compat::SizeFilterBank::mode_count> filters;
    filters[0] = {0.0F, 0.5F};
    filters[1].resize(3001, 0.0F);
    filters[1][3000] = -0.25F;
    filters[2].resize(8192, 0.0F);
    filters[2][8191] = 0.125F;
    filters[3] = {1.0F, -0.5F};

    auto bank = mradm::room_compat::SizeFilterBank::create(filters);
    if (!require(bank.has_value(), "create four-mode size filter bank")) {
        return 1;
    }
    constexpr std::size_t k_frames = 22'000;
    std::vector<float> source(k_frames);
    for (std::size_t frame = 0; frame < k_frames; ++frame) {
        source[frame] = static_cast<float>((frame * 77U) % 251U) / 251.0F - 0.5F;
    }
    std::vector<float> actual(k_frames * filters.size());
    constexpr std::array<std::size_t, 6> sizes{8192, 1300, 4096, 17, 8192, 203};
    std::size_t cursor = 0;
    std::size_t next = 0;
    while (cursor < k_frames) {
        const auto count = std::min(sizes[next % sizes.size()], k_frames - cursor);
        auto input = std::span<const float>{source}.subspan(cursor, count);
        auto output = std::span<float>{actual}.subspan(cursor * filters.size(), count * filters.size());
        if (!require((*bank)->process(input, output).has_value(), "process mixed block sizes")) {
            return 1;
        }
        cursor += count;
        ++next;
    }
    float max_error = 0.0F;
    for (std::size_t frame = 0; frame < k_frames; ++frame) {
        const auto delayed = [&](std::size_t offset) { return frame >= offset ? source[frame - offset] : 0.0F; };
        const std::array<float, 4> expected{
            0.5F * delayed(1), -0.25F * delayed(3000), 0.125F * delayed(8191), delayed(0) - 0.5F * delayed(1)};
        for (std::size_t mode = 0; mode < expected.size(); ++mode) {
            max_error = std::max(max_error, std::fabs(actual[(frame * expected.size()) + mode] - expected[mode]));
        }
    }
    bool ok = require(max_error < 5e-5F, "FFT filtering matches direct FIR across irregular blocks");

    (*bank)->reset();
    std::vector<float> replay(512 * filters.size());
    ok &= require((*bank)->process(std::span<const float>{source}.first(512), replay).has_value(),
                  "filter bank works after reset");
    for (std::size_t sample = 0; sample < replay.size(); ++sample) {
        ok &=
            require(std::fabs(replay[sample] - actual[sample]) < 5e-5F, "reset reproduces the beginning of the stream");
        if (!ok) {
            break;
        }
    }
    auto invalid = filters;
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    ok &= require(!mradm::room_compat::SizeFilterBank::create(invalid), "nonfinite filter is rejected");
    ok &= require(!(*bank)->process(std::span<const float>{source}.first(8193),
                                    std::span<float>{actual}.first(8193 * filters.size())),
                  "oversize process block is rejected");
    return ok ? 0 : 1;
}
