#include "engine_records.h"

#include <algorithm>
#include <iterator>

#include "render_common.h"

namespace mradm::render_common {

std::vector<MradmRenderOverride> override_records(const LiveOverrides& overrides) {
    std::vector<MradmRenderOverride> result;
    result.reserve(overrides.objects.size());
    std::ranges::transform(overrides.objects, std::back_inserter(result), [](const LiveObjectOverride& o) {
        return MradmRenderOverride{text(o.object_id),
                                   text(o.speaker_label),
                                   o.mute ? 1U : 0U,
                                   o.gain_db,
                                   o.head_locked.has_value() ? 1U : 0U,
                                   o.head_locked.value_or(false) ? 1U : 0U};
    });
    return result;
}

std::vector<MradmRenderLiveOverride> live_override_records(const LiveOverrides& overrides) {
    const auto base = override_records(overrides);
    std::vector<MradmRenderLiveOverride> result;
    result.reserve(base.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        const auto& o = overrides.objects[i];
        result.push_back({base[i],
                          o.extent_scale,
                          o.extent_width_scale,
                          o.extent_height_scale,
                          o.extent_depth_scale,
                          o.diffuse_scale,
                          o.divergence_scale});
    }
    return result;
}

MatrixRecords::MatrixRecords(const DirectSpeakersMatrix& matrix) {
    targets.reserve(matrix.routes.size());
    for (const auto& route : matrix.routes) {
        auto& records = targets.emplace_back();
        for (const auto& target : route.targets) {
            records.push_back({text(target.label), target.gain});
            label_bytes += target.label.size();
        }
        routes.push_back({text(route.source_label), route.mute ? 1U : 0U, records.data(), records.size()});
        label_bytes += route.source_label.size();
    }
}

MradmRoomScreenGeometry screen_geometry(const ScreenGeometry& screen) {
    MradmRoomScreenGeometry out{};
    out.cartesian = screen.cartesian ? 1U : 0U;
    out.aspect_ratio = screen.aspect_ratio;
    std::ranges::copy(screen.centre, std::begin(out.centre));
    out.width = screen.width;
    return out;
}

} // namespace mradm::render_common
