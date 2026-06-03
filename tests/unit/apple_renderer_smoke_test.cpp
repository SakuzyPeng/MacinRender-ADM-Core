#include <iostream>
#include <memory>
#include <ranges>
#include <string>

#include "adm/render_apple.h"

namespace {

bool check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
    }
    return cond;
}

bool has_layout(const mradm::CapabilityReport& caps, const std::string& id, uint16_t channels, bool binaural) {
    const auto it = std::ranges::find_if(
        caps.supported_layouts, [&](const mradm::CapabilityReport::Layout& layout) { return layout.id == id; });
    return it != caps.supported_layouts.end() && it->channel_count == channels && it->is_binaural == binaural;
}

bool verify_capabilities() {
    const auto caps = mradm::apple_capabilities();
    bool ok = true;
    ok &= check(caps.backend_name == "apple", "backend name");
    ok &= check(caps.supports_objects, "objects supported");
    ok &= check(caps.supports_direct_speakers, "direct speakers supported");
    ok &= check(!caps.supports_hoa, "hoa unsupported");
    ok &= check(caps.supports_channel_lock, "channel lock preprocessing advertised");
    ok &= check(caps.supports_object_divergence, "object divergence preprocessing advertised");
    ok &= check(!caps.supports_screen_ref, "screenRef unsupported");
    ok &= check(!caps.supports_diffuse, "diffuse unsupported");
    ok &= check(!caps.supports_render_window, "render window unsupported");
    ok &= check(has_layout(caps, "binaural", 2, true), "binaural layout");
    ok &= check(has_layout(caps, "5.1.4", 10, false), "5.1.4 layout");
    ok &= check(has_layout(caps, "7.1.4", 12, false), "7.1.4 layout");
    return ok;
}

bool verify_probe_and_render_placeholder() {
    auto renderer = mradm::create_apple_renderer();
    if (!check(renderer != nullptr, "create_apple_renderer")) {
        return false;
    }

    mradm::RenderPlan plan;
    plan.output_layout = "binaural";
    plan.scene.info.sample_rate = 48000;
    plan.scene.info.num_channels = 1;
    plan.scene.info.num_frames = 480;

    mradm::NullLogSink logs;
    auto prepared = renderer->prepare(plan, logs);
    if (!check(prepared.has_value(), "prepare should initialize AUSpatialMixer")) {
        if (!prepared) {
            std::cerr << "context: " << prepared.error().message << " " << prepared.error().context << "\n";
        }
        return false;
    }

    mradm::NullProgressSink progress;
    auto rendered = renderer->render_window(**prepared, plan, progress, logs);
    if (!check(!rendered.has_value(), "render_window should be placeholder failure")) {
        return false;
    }
    return check(rendered.error().code == mradm::ErrorCode::unsupported, "placeholder error code");
}

} // namespace

int main() {
    bool ok = true;
    ok &= verify_capabilities();
    ok &= verify_probe_and_render_placeholder();
    return ok ? 0 : 1;
}
