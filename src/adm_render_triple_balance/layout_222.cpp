#include "layout_222.h"

#include <algorithm>
#include <iterator>

#include "../adm_dsp/triple_balance.h"
namespace mradm::triple_balance {
bool is_room_222(std::string_view layout) noexcept {
    return layout == "9+10+3" || layout == "22.2";
}
const std::vector<Room222Node>& room_222_nodes() {
    static const auto nodes = [] {
        // Labels remain a control/report mapping. Geometry and filter assignment come from Rust.
        constexpr std::array<std::string_view, 24> labels{
            "M+060", "M-060", "M+000", "LFE1",  "M+135", "M-135", "M+030", "M-030", "M+180", "LFE2",  "M+090", "M-090",
            "U+045", "U-045", "U+000", "T+000", "U+135", "U-135", "U+090", "U-090", "U+180", "B+000", "B+045", "B-045"};
        std::array<MradmTbNode, 22> values{};
        dsp::tb_checked(mradm_dsp_tb_nodes, values.data(), values.size());
        std::vector<Room222Node> result;
        result.reserve(values.size());
        std::ranges::transform(values, std::back_inserter(result), [&](const auto& n) {
            return Room222Node{
                n.channel, labels.at(n.channel), n.position.x, n.position.y, n.position.z, n.filter, n.sign};
        });
        return result;
    }();
    return nodes;
}
Result<Room222Gains> room_222_gains(const SceneBlockPosition& p, float size) {
    if (!p.cartesian) {
        return make_error(ErrorCode::unsupported, "room-222 requires Cartesian coordinates");
    }
    const MradmTbQuery query{{p.x, p.y, p.z}, size, 1, 1};
    Room222Gains result{};
    auto status = dsp::tb_status(mradm_dsp_tb_gains, 2U, &query, std::size_t{1}, result.data(), result.size());
    if (!status) {
        return tl::unexpected{status.error()};
    }
    return result;
}
Room222Mix room_222_mix(const Room222Gains& spatial, float size) {
    std::array<float, 25> values{};
    dsp::tb_checked(mradm_dsp_tb_size_mix, spatial.data(), spatial.size(), size, values.data(), values.size(), 1U);
    Room222Mix result{values[0], {}};
    std::copy(values.begin() + 1, values.end(), result.spread.begin());
    return result;
}
} // namespace mradm::triple_balance
