#include "room_compat_size_filter.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <saf_utility_fft.h>

namespace mradm::room_compat {

namespace {
constexpr std::size_t k_fft_frames = 16'384;
constexpr std::size_t k_fft_bins = (k_fft_frames / 2U) + 1U;
constexpr std::size_t k_overlap_frames = SizeFilterBank::filter_frames - 1U;
} // namespace

SizeFilterBank::SizeFilterBank()
    : source_time_(k_fft_frames, 0.0F), source_fd_(k_fft_bins), output_fd_(k_fft_bins),
      output_time_(k_fft_frames, 0.0F), next_overlap_(k_overlap_frames, 0.0F) {
    for (auto& response : filter_fd_) {
        response.resize(k_fft_bins);
    }
    for (auto& history : overlap_) {
        history.resize(k_overlap_frames, 0.0F);
    }
}

SizeFilterBank::~SizeFilterBank() {
    if (fft_ != nullptr) {
        saf_rfft_destroy(&fft_);
    }
}

Result<std::unique_ptr<SizeFilterBank>>
SizeFilterBank::create(const std::array<std::vector<float>, mode_count>& filters) {
    if (std::ranges::any_of(filters, [](const auto& filter) {
            return filter.empty() || filter.size() > filter_frames ||
                   !std::ranges::all_of(filter, [](float value) { return std::isfinite(value); });
        })) {
        return make_error(ErrorCode::invalid_argument,
                          "size filter modes must contain 1..8192 finite FIR coefficients");
    }
    auto result = std::unique_ptr<SizeFilterBank>(new SizeFilterBank());
    saf_rfft_create(&result->fft_, static_cast<int>(k_fft_frames));
    if (result->fft_ == nullptr) {
        return make_error(ErrorCode::render_failed, "could not create size filter FFT");
    }
    for (std::size_t mode = 0; mode < mode_count; ++mode) {
        std::ranges::fill(result->source_time_, 0.0F);
        std::ranges::copy(filters[mode], result->source_time_.begin());
        saf_rfft_forward(result->fft_, result->source_time_.data(), result->filter_fd_[mode].data());
    }
    std::ranges::fill(result->source_time_, 0.0F);
    return result;
}

Result<void> SizeFilterBank::process(std::span<const float> input, std::span<float> filtered) {
    if (input.size() > block_frames || filtered.size() != input.size() * mode_count) {
        return make_error(ErrorCode::invalid_argument, "size filter requires <=8192 frames and four output modes");
    }
    if (input.empty()) {
        return {};
    }
    std::ranges::fill(source_time_, 0.0F);
    std::ranges::copy(input, source_time_.begin());
    saf_rfft_forward(fft_, source_time_.data(), source_fd_.data());
    for (std::size_t mode = 0; mode < mode_count; ++mode) {
        for (std::size_t band = 0; band < k_fft_bins; ++band) {
            output_fd_[band] = source_fd_[band] * filter_fd_[mode][band];
        }
        saf_rfft_backward(fft_, output_fd_.data(), output_time_.data());
        auto& history = overlap_[mode];
        for (std::size_t frame = 0; frame < input.size(); ++frame) {
            filtered[(frame * mode_count) + mode] =
                output_time_[frame] + (frame < history.size() ? history[frame] : 0.0F);
        }
        for (std::size_t frame = 0; frame < history.size(); ++frame) {
            const float previous = frame + input.size() < history.size() ? history[frame + input.size()] : 0.0F;
            next_overlap_[frame] = previous + output_time_[input.size() + frame];
        }
        history = next_overlap_;
    }
    return {};
}

void SizeFilterBank::reset() noexcept {
    for (auto& history : overlap_) {
        std::ranges::fill(history, 0.0F);
    }
}

} // namespace mradm::room_compat
