#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

#include "adm/render_triple_balance.h"
#include "adm/scene.h"

#include "panner.h"

namespace {

template <std::size_t N>
bool check_reference(std::string_view layout, std::array<float, 3> xyz, const std::array<float, N>& reference) {
    mradm::SceneBlockPosition position;
    position.cartesian = true;
    position.x = xyz[0];
    position.y = xyz[1];
    position.z = xyz[2];
    const auto result = mradm::triple_balance::point_gains(position, 1.0F, layout);
    if (!result || result->size() != reference.size()) {
        std::cerr << "wrong result size for " << layout << '\n';
        return false;
    }
    double error_power = 0.0;
    double reference_power = 0.0;
    for (std::size_t channel = 0; channel < N; ++channel) {
        const double difference = static_cast<double>((*result)[channel]) - reference.at(channel);
        error_power += difference * difference;
        reference_power += static_cast<double>(reference.at(channel)) * reference.at(channel);
        if (!std::isfinite((*result)[channel])) {
            return false;
        }
    }
    if ((*result)[3] != 0.0F || std::sqrt(error_power / reference_power) > 0.01) {
        std::cerr << "triple-balance point reference mismatch for " << layout << '\n';
        return false;
    }
    return true;
}

} // namespace

int main() {
    // Measured from the held-out direct-ADM reference bank, in named
    // multi-mono speaker order. These coordinates were not used to fit the
    // room grid.
    bool ok = check_reference("7.1.4",
                              {0.3395F, 0.4342F, 0.9594F}, // NOLINT(modernize-use-std-numbers): measured coordinates.
                              std::array{0.0F,
                                         0.02018273F,
                                         0.033891F,
                                         0.0F,
                                         0.02414826F,
                                         0.04248826F,
                                         0.0F,
                                         0.0F,
                                         0.25934714F,
                                         0.95534481F,
                                         0.03325371F,
                                         0.12249515F});
    ok &= check_reference("9.1.6",
                          {-0.8526F, -0.7582F, 0.4199F},
                          std::array{0.0F,
                                     0.0F,
                                     0.0F,
                                     0.0F,
                                     0.28975839F,
                                     0.03392162F,
                                     0.73615687F,
                                     0.08618087F,
                                     0.0F,
                                     0.0F,
                                     0.0F,
                                     0.0F,
                                     0.0F,
                                     0.0F,
                                     0.604595F,
                                     0.0F});
    mradm::SceneBlockPosition invalid;
    invalid.cartesian = true;
    invalid.x = 1.1F;
    invalid.y = 0.0F;
    invalid.z = 0.0F;
    ok &= !mradm::triple_balance::point_gains(invalid, 1.0F, "7.1.4");
    ok &= !mradm::triple_balance::point_gains(invalid, 1.0F, "22.2");
    auto renderer = mradm::create_triple_balance_renderer();
    const auto caps = renderer->capabilities();
    ok &= caps.backend_name == "triple-balance" && caps.supported_layouts.size() == 3;
    ok &= caps.supports_objects && caps.supports_direct_speakers && caps.supports_render_window;
    ok &= !caps.supports_channel_lock && !caps.supports_object_divergence && !caps.supports_hoa;
    for (const auto* id : {"4+7+0", "9.1.6", "9+10+3"}) {
        ok &= std::ranges::any_of(caps.supported_layouts, [id](const auto& layout) { return layout.id == id; });
    }
    if (!ok) {
        std::cerr << "Triple Balance reference or capability regression\n";
    }
    return ok ? 0 : 1;
}
