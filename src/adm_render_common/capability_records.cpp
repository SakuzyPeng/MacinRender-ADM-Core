#include "capability_records.h"

#include <span>
#include <stdexcept>
#include <string>

#include "engine_ffi.h"

namespace mradm::render_common {

namespace {

std::string copy(MradmAdmText text) {
    return text.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(text.data), text.len};
}

std::vector<CapabilityReport::Mode> modes(const MradmEngineMode* data, std::size_t len) {
    std::vector<CapabilityReport::Mode> out;
    for (const auto& m : std::span{data, len}) {
        CapabilityReport::Mode mode{copy(m.id), copy(m.display_name), {}, {}, m.supports_realtime != 0};
        for (const auto& layout : std::span{m.layouts, m.layouts_len}) {
            mode.layouts.push_back(copy(layout));
        }
        const std::span rates{m.sample_rates, m.sample_rates_len};
        mode.sample_rates.assign(rates.begin(), rates.end());
        out.push_back(std::move(mode));
    }
    return out;
}

} // namespace

CapabilityReport engine_capabilities(EngineBackend backend) {
    const MradmEngineBackend* record = nullptr;
    if (mradm_engine_backend(static_cast<uint32_t>(backend), &record) != 0 || record == nullptr) {
        throw std::runtime_error("backend capability record is unavailable");
    }
    CapabilityReport r;
    r.backend_name = copy(record->name);
    r.backend_version = copy(record->version);
    for (const auto& l : std::span{record->layouts, record->layouts_len}) {
        r.supported_layouts.push_back({copy(l.id),
                                       copy(l.display_name),
                                       l.channel_count,
                                       (l.flags & 1U) != 0,
                                       l.lfe_count,
                                       (l.flags & 2U) != 0,
                                       (l.flags & 4U) != 0});
    }
    r.modes = modes(record->modes, record->modes_len);
    r.qualities = modes(record->qualities, record->qualities_len);
    const auto flag = [record](uint32_t bit) { return (record->flags & (1U << bit)) != 0; };
    r.supports_objects = flag(0);
    r.supports_direct_speakers = flag(1);
    r.supports_hoa = flag(2);
    r.supports_channel_lock = flag(3);
    r.supports_object_divergence = flag(4);
    r.supports_screen_ref = flag(5);
    r.supports_diffuse = flag(6);
    r.supports_render_window = flag(7);
    for (const auto& source : std::span{record->hrtf_sources, record->hrtf_sources_len}) {
        r.hrtf_sources.push_back(copy(source));
    }
    return r;
}

} // namespace mradm::render_common
