#include "platform_island.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "adm/capability.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif

namespace mradm::engine {
namespace {

#ifdef __APPLE__
MradmAdmText text(std::string_view value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// Borrowed records of a C++ capability report (the Apple island); they point into `report`.
class BackendRecords {
  public:
    explicit BackendRecords(CapabilityReport report) : report_(std::move(report)) {
        for (const auto& l : report_.supported_layouts) {
            const uint32_t flags = (l.is_3d ? 1U : 0U) | (l.supports_spread ? 2U : 0U) | (l.is_binaural ? 4U : 0U);
            layouts_.push_back({text(l.id), text(l.display_name), l.channel_count, l.lfe_count, flags});
        }
        modes_ = modes(report_.modes);
        qualities_ = modes(report_.qualities);
        hrtf_.resize(report_.hrtf_sources.size());
        std::ranges::transform(report_.hrtf_sources, hrtf_.begin(), text);
        const std::array bits{report_.supports_objects,
                              report_.supports_direct_speakers,
                              report_.supports_hoa,
                              report_.supports_channel_lock,
                              report_.supports_object_divergence,
                              report_.supports_screen_ref,
                              report_.supports_diffuse,
                              report_.supports_render_window};
        uint32_t flags = 0;
        uint32_t bit = 0;
        for (const bool set : bits) {
            flags |= set ? (1U << bit) : 0U;
            ++bit;
        }
        record_ = {text("apple"),
                   text(report_.backend_name),
                   text(report_.backend_version),
                   layouts_.data(),
                   layouts_.size(),
                   modes_.data(),
                   modes_.size(),
                   qualities_.data(),
                   qualities_.size(),
                   hrtf_.data(),
                   hrtf_.size(),
                   flags};
    }
    BackendRecords(const BackendRecords&) = delete;
    BackendRecords& operator=(const BackendRecords&) = delete;
    BackendRecords(BackendRecords&&) = delete;
    BackendRecords& operator=(BackendRecords&&) = delete;
    ~BackendRecords() = default;

    [[nodiscard]] const MradmEngineBackend* get() const { return &record_; }

  private:
    std::vector<MradmEngineMode> modes(const std::vector<CapabilityReport::Mode>& source) {
        std::vector<MradmEngineMode> out;
        for (const auto& mode : source) {
            auto& names = mode_layouts_.emplace_back();
            names.resize(mode.layouts.size());
            std::ranges::transform(mode.layouts, names.begin(), text);
            out.push_back({text(mode.id),
                           text(mode.display_name),
                           names.data(),
                           names.size(),
                           mode.sample_rates.data(),
                           mode.sample_rates.size(),
                           mode.supports_realtime ? 1U : 0U});
        }
        return out;
    }

    CapabilityReport report_;
    std::vector<MradmEngineLayout> layouts_;
    std::vector<std::vector<MradmAdmText>> mode_layouts_;
    std::vector<MradmEngineMode> modes_;
    std::vector<MradmEngineMode> qualities_;
    std::vector<MradmAdmText> hrtf_;
    MradmEngineBackend record_{};
};

const BackendRecords& apple_records() {
    static const BackendRecords records{apple_capabilities()};
    return records;
}

const MradmEngineBackend* apple() noexcept {
    return apple_records().get();
}

// Runtime self-test: macOS <=26.3's AUSpatialMixer misroutes a bypass-LFE source to Center.
int32_t apple_lfe_routing_ok() noexcept {
    return apple_system_spatial_lfe_routing_ok() ? 1 : 0;
}

// AVSampleBufferAudioRenderer accepts the Apple speaker layouts.
void system_spatial(const MradmEngineSpatialLayout** layouts, size_t* len) noexcept {
    static const CapabilityReport caps = apple_capabilities();
    static const std::vector<MradmEngineSpatialLayout> records = [] {
        std::vector<MradmEngineSpatialLayout> out;
        for (const auto& layout : caps.supported_layouts) {
            if (!layout.is_binaural) {
                out.push_back({text(layout.id), text(layout.display_name), layout.channel_count, 0});
            }
        }
        return out;
    }();
    *layouts = records.data();
    *len = records.size();
}

constexpr MradmPlatformIslands k_platform{&apple, &apple_lfe_routing_ok, &system_spatial};
#else
constexpr MradmPlatformIslands k_platform{};
#endif

} // namespace
} // namespace mradm::engine

extern "C" const MradmPlatformIslands* mradm_platform_islands(void) {
    return &mradm::engine::k_platform;
}
