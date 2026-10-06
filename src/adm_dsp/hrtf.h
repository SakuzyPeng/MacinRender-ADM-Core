#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "adm/errors.h"

#include "dsp.h"

namespace mradm::dsp {

// Snapshot copies are explicitly requested by tests/diagnostics. Rendering
// retains the tables in Rust and shares them without a second C++ allocation.
struct HrtfGridSnapshot {
    std::vector<float> gains;
    std::vector<std::int32_t> directions;
};

class HrtfGrid final {
  public:
    [[nodiscard]] static Result<HrtfGrid> create(std::span<const float> directions) {
        std::array<char, 256> message{};
        void* handle = nullptr;
        auto status =
            mradm_dsp_hrtf_grid_create(directions.data(), directions.size(), &handle, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF grid");
        }
        HrtfGrid result(handle);
        status = mradm_dsp_hrtf_grid_info(handle, &result.info_, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF grid");
        }
        return result;
    }
    [[nodiscard]] std::size_t bytes() const noexcept { return sizeof(*this) + info_.storage_bytes; }
    [[nodiscard]] Result<HrtfGridSnapshot> snapshot() const {
        HrtfGridSnapshot result;
        result.gains.resize(info_.entries);
        result.directions.resize(info_.entries);
        std::array<char, 256> message{};
        const auto status = mradm_dsp_hrtf_grid_copy(handle_.get(),
                                                     result.gains.data(),
                                                     result.directions.data(),
                                                     info_.entries,
                                                     message.data(),
                                                     message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF grid snapshot");
        }
        return result;
    }

  private:
    friend class HrtfFilters;
    explicit HrtfGrid(void* handle) : handle_(handle, mradm_dsp_hrtf_grid_destroy) {}
    std::unique_ptr<void, decltype(&mradm_dsp_hrtf_grid_destroy)> handle_;
    MradmDspHrtfGridInfo info_{};
};

enum class HrtfLookup : std::uint32_t { quantized, continuous };

// Preparation owns all allocations. Each query reads an immutable Rust bank
// and writes the caller's prepared buffer; banks may be shared across workers.
class HrtfFilters final {
  public:
    HrtfFilters() = default;
    [[nodiscard]] static Result<HrtfFilters> create(const HrtfGrid& grid,
                                                    std::span<const float> impulses,
                                                    std::size_t taps,
                                                    std::size_t fft_length,
                                                    bool cache_magnitudes) {
        std::array<char, 256> message{};
        void* handle = nullptr;
        auto status = mradm_dsp_hrtf_filters_create(grid.handle_.get(),
                                                    impulses.data(),
                                                    impulses.size(),
                                                    taps,
                                                    fft_length,
                                                    cache_magnitudes ? 1U : 0U,
                                                    &handle,
                                                    message.data(),
                                                    message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF filters");
        }
        HrtfFilters result;
        result.handle_.reset(handle);
        status = mradm_dsp_hrtf_filters_info(handle, &result.info_, message.data(), message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF filters");
        }
        return result;
    }
    [[nodiscard]] const void* get() const noexcept { return handle_.get(); }
    [[nodiscard]] std::size_t output_size() const noexcept { return info_.output_length / 2U; }
    [[nodiscard]] std::size_t bytes() const noexcept { return info_.storage_bytes; }
    [[nodiscard]] Result<void> query(float azimuth,
                                     float elevation,
                                     HrtfLookup mode,
                                     std::span<Complex> output,
                                     const MradmDspHrtfTrace* trace = nullptr) const {
        std::array<char, 256> message{};
        const auto status = mradm_dsp_hrtf_query(handle_.get(),
                                                 azimuth,
                                                 elevation,
                                                 static_cast<std::uint32_t>(mode),
                                                 reinterpret_cast<float*>(output.data()),
                                                 output.size() * 2U,
                                                 trace,
                                                 message.data(),
                                                 message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF interpolation");
        }
        return {};
    }
    [[nodiscard]] Result<std::vector<Complex>> spectrum_snapshot() const {
        std::vector<Complex> result(info_.spectrum_length / 2U);
        std::array<char, 256> message{};
        const auto status = mradm_dsp_hrtf_spectra_copy(handle_.get(),
                                                        reinterpret_cast<float*>(result.data()),
                                                        info_.spectrum_length,
                                                        message.data(),
                                                        message.size());
        if (status != 0) {
            return make_error(static_cast<ErrorCode>(status), message.data(), "HRTF spectrum snapshot");
        }
        return result;
    }

  private:
    std::unique_ptr<void, decltype(&mradm_dsp_hrtf_filters_destroy)> handle_{nullptr, mradm_dsp_hrtf_filters_destroy};
    MradmDspHrtfFilterInfo info_{};
};
} // namespace mradm::dsp
