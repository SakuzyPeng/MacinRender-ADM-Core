#include "size_panner.h"

#include <algorithm>

#include "../adm_dsp/triple_balance.h"
namespace mradm::triple_balance {
Result<QuantizedSizeParameters> quantize_size_parameters(SizePosition p, float size) {
    std::array<int32_t, 4> q{};
    auto status = dsp::tb_status(mradm_dsp_tb_quantize, MradmTbPosition{p.x, p.y, p.z}, size, q.data(), q.size());
    if (!status) {
        return make_error(ErrorCode::unsupported, status.error().message);
    }
    return QuantizedSizeParameters{{q[0], q[1], q[2]}, q[3]};
}
Result<SizeGains> raw_size_gains(const QuantizedSizeParameters& p) {
    const std::array q{p.xyz[0], p.xyz[1], p.xyz[2], p.size};
    SizeGains result{};
    auto status = dsp::tb_status(mradm_dsp_tb_raw, q.data(), q.size(), result.data(), result.size());
    if (!status) {
        return make_error(ErrorCode::unsupported, status.error().message);
    }
    return result;
}
SizeMixGains mix_size_gains(const SizeGains& raw, float size) noexcept {
    std::array<float, 12> values{};
    dsp::tb_checked(mradm_dsp_tb_size_mix, raw.data(), raw.size(), size, values.data(), values.size(), 0U);
    SizeMixGains result{values[0], {}};
    std::copy(values.begin() + 1, values.end(), result.spread.begin());
    return result;
}
} // namespace mradm::triple_balance
