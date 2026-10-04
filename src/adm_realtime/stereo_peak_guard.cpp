#include "stereo_peak_guard.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

#include "dsp_ffi.h"

namespace mradm::realtime {
namespace {
void check(int status) noexcept {
    if (status != 0) {
        std::terminate();
    }
}
MradmDspPeakStatus status(const void* handle, bool ended = false) noexcept {
    MradmDspPeakStatus result{};
    check(mradm_dsp_peak_guard_status(handle, ended ? 1U : 0U, &result, nullptr, 0U));
    return result;
}
} // namespace

StereoPeakGuard::StereoPeakGuard(std::uint32_t sample_rate) {
    std::array<char, 256> error{};
    if (mradm_dsp_peak_guard_create(sample_rate, &handle_, error.data(), error.size()) != 0) {
        throw std::runtime_error(error.data());
    }
}
StereoPeakGuard::~StereoPeakGuard() {
    mradm_dsp_peak_guard_destroy(handle_);
}
StereoPeakGuard::StereoPeakGuard(StereoPeakGuard&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
StereoPeakGuard& StereoPeakGuard::operator=(StereoPeakGuard&& other) noexcept {
    if (this != &other) {
        mradm_dsp_peak_guard_destroy(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}
void StereoPeakGuard::reset() noexcept {
    check(mradm_dsp_peak_guard_reset(handle_, nullptr, 0U));
}
std::size_t StereoPeakGuard::lookahead_frames() const noexcept {
    return status(handle_).lookahead;
}
std::size_t StereoPeakGuard::buffered_frames() const noexcept {
    return status(handle_).buffered;
}
std::size_t StereoPeakGuard::writable_frames() const noexcept {
    return status(handle_).writable;
}
std::size_t StereoPeakGuard::readable_frames(bool ended) const noexcept {
    return status(handle_, ended).readable;
}
void StereoPeakGuard::push(std::span<const float> stereo) noexcept {
    check(mradm_dsp_peak_guard_push(handle_, stereo.data(), stereo.size(), nullptr, 0U));
}
std::size_t StereoPeakGuard::pop(std::span<float> output, float volume, bool ended) noexcept {
    std::size_t frames = 0;
    check(
        mradm_dsp_peak_guard_pop(handle_, output.data(), output.size(), volume, ended ? 1U : 0U, &frames, nullptr, 0U));
    return frames;
}
} // namespace mradm::realtime
