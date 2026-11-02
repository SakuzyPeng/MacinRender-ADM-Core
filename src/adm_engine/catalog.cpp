#include "catalog.h"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

#include "adm/audio_io.h"
#include "adm/capability.h"
#ifdef __APPLE__
#include "adm/render_apple.h"
#endif
#ifdef _WIN32
#include "audio_output_device.h"
#endif

#include "engine_ffi.h"

namespace mradm::engine {

namespace {

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

} // namespace

std::string document(Document kind) {
    MradmEnginePlatform platform{0, 0, 0, -1, nullptr, nullptr, 0};
    if (kind == Document::output_formats || kind == Document::support_matrix) {
        platform.apac = audio::apac_encoding_available() ? 1U : 0U;
        platform.iamf = audio::iamf_encoding_available() ? 1U : 0U;
        // The packager probe runs only in IAMF builds.
        platform.iamf_mp4_packager = platform.iamf != 0U && audio::iamf_mp4_packager_available() ? 1U : 0U;
    }
    std::vector<MradmEngineSpatialLayout> spatial_records;
#ifdef __APPLE__
    const CapabilityReport apple_caps = apple_capabilities();
    if (kind == Document::capabilities) {
        // macOS system-spatial monitoring (AVSampleBufferAudioRenderer) accepts Apple's speaker layouts.
        for (const auto& layout : apple_caps.supported_layouts) {
            if (!layout.is_binaural) {
                spatial_records.push_back({text(layout.id), text(layout.display_name), layout.channel_count, 0});
            }
        }
        // Runtime self-test: macOS <=26.3's AUSpatialMixer misroutes a bypass-LFE source to Center.
        platform.apple_lfe_routing_ok = apple_system_spatial_lfe_routing_ok() ? 1 : 0;
    }
    const BackendRecords apple{apple_caps};
    platform.apple = apple.get();
#elif defined(_WIN32)
    const auto spatial = realtime::system_spatial_layouts();
    if (kind == Document::capabilities) {
        spatial_records.resize(spatial.size());
        std::ranges::transform(spatial, spatial_records.begin(), [](const auto& layout) {
            return MradmEngineSpatialLayout{
                text(layout.id), text(layout.display_name), static_cast<uint16_t>(layout.channel_count), 0};
        });
    }
#endif
    platform.system_spatial = spatial_records.data();
    platform.system_spatial_len = spatial_records.size();

    MradmEngineText* raw = nullptr;
    MradmAdmText json{};
    if (mradm_engine_document(static_cast<uint32_t>(kind), &platform, &raw, &json) != 0) {
        throw std::runtime_error("discovery document is unavailable");
    }
    const std::unique_ptr<MradmEngineText, decltype(&mradm_engine_text_destroy)> owned(raw, &mradm_engine_text_destroy);
    return {reinterpret_cast<const char*>(json.data), json.len};
}

class RenderPlatform::Apple {
  public:
    explicit Apple(CapabilityReport report) : records_(std::move(report)) {}
    [[nodiscard]] const MradmEngineBackend* get() const { return records_.get(); }

  private:
    BackendRecords records_;
};

RenderPlatform::RenderPlatform() {
    platform_.iamf = audio::iamf_encoding_available() ? 1U : 0U;
#ifdef __APPLE__
    apple_ = std::make_unique<Apple>(apple_capabilities());
    platform_.apple = apple_->get();
#endif
}

RenderPlatform::~RenderPlatform() = default;

std::vector<OutputLayoutRow> build_output_layouts() {
    const auto root = nlohmann::json::parse(document(Document::layouts));
    std::vector<OutputLayoutRow> rows(root.at("layouts").size());
    std::ranges::transform(root.at("layouts"), rows.begin(), [](const nlohmann::json& j) -> OutputLayoutRow {
        return {j.at("format").get<std::string>(),
                j.at("layout").get<std::string>(),
                j.at("channels").get<uint32_t>(),
                j.at("container").get<std::string>(),
                j.at("order").get<std::string>(),
                j.value("note", std::string{}),
                j.at("supported_by").get<std::vector<std::string>>()};
    });
    return rows;
}

OutputFormats build_output_formats() {
    const auto root = nlohmann::json::parse(document(Document::output_formats));
    OutputFormats out;
    const auto& f = root.at("features");
    out.features = {.apac = f.at("apac").get<bool>(),
                    .iamf = f.at("iamf").get<bool>(),
                    .iamf_mp4_packager = f.at("iamf_mp4_packager").get<bool>(),
                    .sofa = f.at("sofa").get<bool>(),
                    .hptf = f.at("hptf").get<bool>()};
    for (const auto& j : root.at("formats")) {
        OutputFormatInfo info;
        info.format = j.at("format").get<std::string>();
        info.extensions = j.at("extensions").get<std::vector<std::string>>();
        info.available = j.at("available").get<bool>();
        info.available_reason = j.value("available_reason", std::string{});
        info.lossy = j.at("lossy").get<bool>();
        info.max_channels = j.at("max_channels").get<uint32_t>();
        info.fixed_sample_rate = j.at("fixed_sample_rate").get<uint32_t>();
        info.supports_height = j.at("supports_height").get<bool>();
        info.bit_depths = j.value("bit_depths", std::vector<std::string>{});
        info.note = j.value("note", std::string{});
        for (const auto* key : {"bitrate_kbps_per_ch", "bitrate_kbps_total"}) {
            if (j.contains(key)) {
                info.has_bitrate = true;
                info.bitrate_per_channel = std::string_view{key} == "bitrate_kbps_per_ch";
                info.bitrate_min_kbps = j.at(key).at("min").get<uint32_t>();
                info.bitrate_max_kbps = j.at(key).at("max").get<uint32_t>();
            }
        }
        out.formats.push_back(std::move(info));
    }
    return out;
}

} // namespace mradm::engine
