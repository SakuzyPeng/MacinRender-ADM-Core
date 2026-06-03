#include <algorithm>
#include <string>

#include <fmt/format.h>

#ifdef __APPLE__
#include "adm/render_apple.h"
#endif
#include "adm/render_binaural.h"
#include "adm/render_ear.h"
#include "adm/render_hoa.h"
#include "adm/render_vbap.h"

#include "commands.h"

namespace {

void print_capabilities(const mradm::CapabilityReport& caps) {
    auto layout_label = [](const std::string& id) {
        if (id == "0+2+0") {
            return std::string{"stereo"};
        }
        if (id == "0+5+0") {
            return std::string{"5.1"};
        }
        if (id == "2+5+0") {
            return std::string{"5.1.2"};
        }
        if (id == "wav71") {
            return std::string{"7.1"};
        }
        if (id == "4+5+0") {
            return std::string{"5.1.4"};
        }
        if (id == "4+5+4") {
            return std::string{"9.1.4"};
        }
        if (id == "4+7+0") {
            return std::string{"7.1.4"};
        }
        if (id == "9+10+3") {
            return std::string{"22.2"};
        }
        return id;
    };

    fmt::print("Backend: {} {}\n", caps.backend_name, caps.backend_version);
    fmt::print("  Objects:        {}\n", caps.supports_objects ? "yes" : "no");
    fmt::print("  DirectSpeakers: {}\n", caps.supports_direct_speakers ? "yes" : "no");
    fmt::print("  HOA:            {}\n", caps.supports_hoa ? "yes" : "no");
    fmt::print("  ChannelLock:    {}\n", caps.supports_channel_lock ? "yes" : "no");
    fmt::print("  Divergence:     {}\n", caps.supports_object_divergence ? "yes" : "no");
    fmt::print("  ScreenRef:      {}\n", caps.supports_screen_ref ? "yes" : "no");
    fmt::print("  Diffuse:        {}\n", caps.supports_diffuse ? "yes" : "no");
    const auto visible_layouts = std::ranges::count_if(
        caps.supported_layouts, [](const auto& layout) { return layout.is_binaural || layout.channel_count != 2U; });
    fmt::print("  Layouts ({}):\n", visible_layouts);
    for (const auto& layout : caps.supported_layouts) {
        if (!layout.is_binaural && layout.channel_count == 2U) {
            continue;
        }
        std::string flags;
        if (layout.channel_count > 0) {
            flags += fmt::format("{}ch", layout.channel_count);
        }
        if (layout.lfe_count > 0) {
            flags += fmt::format(" {}lfe", layout.lfe_count);
        }
        if (layout.is_3d) {
            flags += " 3d";
        }
        if (layout.supports_spread) {
            flags += " spread";
        }
        if (layout.is_binaural) {
            flags += " binaural";
        }
        const std::string label = layout.is_binaural ? std::string{"binaural"} : layout_label(layout.id);
        fmt::print("    {:<12}  {:<30}  {}\n", label, layout.display_name, flags);
    }
}

void print_all_capabilities() {
    print_capabilities(mradm::ear_capabilities());
    fmt::print("\n");
    print_capabilities(mradm::vbap_capabilities());
    fmt::print("\n");
    print_capabilities(mradm::hoa_capabilities());
    fmt::print("\n");
    print_capabilities(mradm::binaural_capabilities());
#ifdef __APPLE__
    fmt::print("\n");
    print_capabilities(mradm::apple_capabilities());
#endif
}

} // namespace

CLI::App* add_backends_command(CLI::App& app) {
    return app.add_subcommand("backends", "List available renderer backends and supported layouts");
}

void run_backends() {
    print_all_capabilities();
}
