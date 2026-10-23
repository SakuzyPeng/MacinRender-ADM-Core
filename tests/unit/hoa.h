#pragma once
#include <array>
#include <exception>
#include <memory>
#include <span>

#include "adm/errors.h"

#include "hoa_ffi.h"
namespace mradm::dsp {
template <class F, class... Args> Result<void> hoa_status(F function, Args... args) {
    std::array<char, 256> error{};
    const int status = function(args..., error.data(), error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data(), "HOA DSP");
    }
    return {};
}
template <class F, class... Args> void hoa_checked(F function, Args... args) noexcept {
    std::array<char, 256> error{};
    if (function(args..., error.data(), error.size()) != 0) {
        std::terminate();
    }
}
template <auto Destroy> using HoaHandle = std::unique_ptr<void, decltype(Destroy)>;
class HoaPlan {
  public:
    static Result<HoaPlan> create(std::size_t inputs,
                                  std::span<const MradmHoaRow> rows,
                                  std::span<const MradmHoaBlock> blocks,
                                  std::span<const std::size_t> order,
                                  std::span<const MradmHoaSource> sources,
                                  MradmHoaTrace* trace = nullptr) {
        HoaPlan result;
        void* raw = nullptr;
        auto status = hoa_status(mradm_dsp_hoa_plan_create,
                                 inputs,
                                 rows.data(),
                                 rows.size(),
                                 blocks.data(),
                                 blocks.size(),
                                 order.data(),
                                 order.size(),
                                 sources.data(),
                                 sources.size(),
                                 &raw,
                                 trace);
        if (!status) {
            return tl::unexpected{status.error()};
        }
        result.handle_.reset(raw);
        return result;
    }
    const void* get() const noexcept { return handle_.get(); }
    bool has_lfe() const noexcept {
        uint32_t value = 0;
        hoa_checked(mradm_dsp_hoa_plan_has_lfe, get(), &value);
        return value != 0;
    }

  private:
    HoaHandle<mradm_dsp_hoa_plan_destroy> handle_{nullptr, mradm_dsp_hoa_plan_destroy};
};
class HoaEncoder {
  public:
    static Result<HoaEncoder>
    create(const HoaPlan& plan, std::size_t max_frames, uint64_t interpolation, bool smoothing) {
        HoaEncoder result;
        void* raw = nullptr;
        auto status =
            hoa_status(mradm_dsp_hoa_encoder_create, plan.get(), max_frames, interpolation, smoothing ? 1U : 0U, &raw);
        if (!status) {
            return tl::unexpected{status.error()};
        }
        result.handle_.reset(raw);
        return result;
    }
    void reset(uint64_t start) { hoa_checked(mradm_dsp_hoa_encoder_reset, handle_.get(), start); }
    Result<void> process(std::span<const float> input, std::span<float> output, uint64_t start, std::size_t frames) {
        return hoa_status(mradm_dsp_hoa_encode,
                          handle_.get(),
                          input.data(),
                          input.size(),
                          output.data(),
                          output.size(),
                          start,
                          frames);
    }

  private:
    HoaHandle<mradm_dsp_hoa_encoder_destroy> handle_{nullptr, mradm_dsp_hoa_encoder_destroy};
};
class HoaMeterPreprocessor {
  public:
    static Result<HoaMeterPreprocessor> create(const HoaPlan& plan, std::size_t max_frames, uint64_t interpolation) {
        HoaMeterPreprocessor result;
        void* raw = nullptr;
        auto status = hoa_status(mradm_dsp_hoa_meter_create, plan.get(), max_frames, interpolation, &raw);
        if (!status) {
            return tl::unexpected{status.error()};
        }
        result.handle_.reset(raw);
        return result;
    }
    void reset(uint64_t start) { hoa_checked(mradm_dsp_hoa_meter_reset, handle_.get(), start); }
    Result<void> process(std::span<const float> input,
                         std::span<const float> encoded,
                         std::span<float> decoded,
                         std::span<float> lfe,
                         uint64_t start,
                         std::size_t frames) {
        return hoa_status(mradm_dsp_hoa_meter_process,
                          handle_.get(),
                          input.data(),
                          input.size(),
                          encoded.data(),
                          encoded.size(),
                          decoded.data(),
                          decoded.size(),
                          lfe.data(),
                          lfe.size(),
                          start,
                          frames);
    }

  private:
    HoaHandle<mradm_dsp_hoa_meter_destroy> handle_{nullptr, mradm_dsp_hoa_meter_destroy};
};
} // namespace mradm::dsp
