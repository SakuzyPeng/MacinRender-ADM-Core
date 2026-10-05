#include "panner.h"

#include <cmath>

#include "../adm_dsp/triple_balance.h"
namespace mradm::triple_balance {
Result<std::vector<float>> point_gains(const SceneBlockPosition& p, float gain, std::string_view layout) {
    auto code = dsp::tb_layout(layout);
    if (!code) {
        return tl::unexpected{code.error()};
    }
    if (!p.cartesian || !std::isfinite(gain)) {
        return make_error(ErrorCode::unsupported, "Triple Balance requires finite Cartesian position and gain");
    }
    const MradmTbQuery query{{p.x, p.y, p.z}, 0, gain, 0};
    std::array<float, 24> values{};
    auto status = dsp::tb_status(mradm_dsp_tb_gains, *code, &query, std::size_t{1}, values.data(), values.size());
    if (!status) {
        return tl::unexpected{status.error()};
    }
    const auto channels = static_cast<std::ptrdiff_t>(dsp::tb_channels(*code));
    return std::vector<float>(values.begin(), values.begin() + channels);
}
} // namespace mradm::triple_balance
