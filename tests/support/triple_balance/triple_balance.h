#pragma once
#include <array>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "adm/errors.h"

#include "pcm_mix.h"
#include "triple_balance_ffi.h"

namespace mradm::dsp {
template <class F, class... A> Result<void> tb_status(F function, A... args) {
    std::array<char, 256> message{};
    const auto code = function(args..., message.data(), message.size());
    if (code != 0) {
        return make_error(static_cast<ErrorCode>(code), message.data(), "Triple Balance DSP");
    }
    return {};
}
// Runtime calls here have already validated private preconditions (ADR 0005).
template <class F, class... A> void tb_checked(F function, A... args) noexcept {
    std::array<char, 256> message{};
    if (function(args..., message.data(), message.size()) != 0) {
        std::terminate();
    }
}
inline Result<uint32_t> tb_layout(std::string_view layout) {
    if (layout == "7.1.4" || layout == "4+7+0") {
        return 0U;
    }
    if (layout == "9.1.6") {
        return 1U;
    }
    if (layout == "22.2" || layout == "9+10+3") {
        return 2U;
    }
    return make_error(ErrorCode::unsupported, "Unsupported Triple Balance layout");
}
inline std::size_t tb_channels(uint32_t code) {
    constexpr std::array<std::size_t, 3> counts{12, 16, 24};
    return counts.at(code);
}
template <auto Destroy> using TbHandle = std::unique_ptr<void, decltype(Destroy)>;
using TbSnapshot = TbHandle<mradm_dsp_tb_snapshot_destroy>;
class TbPlan {
  public:
    static Result<TbPlan> create(std::size_t inputs,
                                 uint32_t layout,
                                 uint32_t rate,
                                 uint64_t total,
                                 std::span<const MradmTbRow> rows,
                                 std::span<const MradmTbEvent> events,
                                 std::span<const float> bed) {
        TbPlan result;
        void* raw = nullptr;
        auto status = tb_status(mradm_dsp_tb_plan_create,
                                inputs,
                                layout,
                                rate,
                                total,
                                rows.data(),
                                rows.size(),
                                events.data(),
                                events.size(),
                                bed.data(),
                                bed.size(),
                                &raw);
        if (!status) {
            return tl::unexpected{status.error()};
        }
        result.handle_.reset(raw);
        return result;
    }
    Result<PcmMixPlan> mix_plan() const {
        void* raw = nullptr;
        auto status = tb_status(mradm_dsp_tb_plan_mix, get(), &raw);
        if (!status) {
            return tl::unexpected{status.error()};
        }
        return PcmMixPlan::adopt(raw);
    }
    const void* get() const noexcept { return handle_.get(); }

  private:
    TbHandle<mradm_dsp_tb_plan_destroy> handle_{nullptr, mradm_dsp_tb_plan_destroy};
};
} // namespace mradm::dsp
