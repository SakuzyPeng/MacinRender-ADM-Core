#include <cmath>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "binaural_internal.h"

namespace {
bool check(bool value, std::string_view message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    using mradm::binaural_internal::load_sofa_dataset;
    const std::filesystem::path root{argv[1]};
    bool ok = true;
#if MR_ADM_ENABLE_SOFA
    for (const auto* filename : {"simple.sofa", "general-compressed.sofa"}) {
        auto data = load_sofa_dataset(root / filename, 0);
        if (!check(data.has_value(), filename)) {
            return 1;
        }
        ok &= check(data->num_dirs == 6 && data->hrir_len == 16, "SOFA dimensions");
        const int rate = std::string_view{filename} == "simple.sofa" ? 48000 : 44100;
        ok &= check(data->sample_rate == rate, "native sample rate is retained");
        ok &= check(data->name == "SOFA MacinRender synthetic", "SOFA listener name");
        for (std::size_t dir = 0; dir < 6U; ++dir) {
            ok &= check(data->hrirs[((dir * 2U) * 16U) + 2U] == static_cast<float>(dir + 1U) / 8.0F,
                        "left HRIR is not normalized");
            ok &= check(data->hrirs[(((dir * 2U) + 1U) * 16U) + 4U] == static_cast<float>(6U - dir) / 8.0F,
                        "right HRIR is not normalized");
        }
        auto state = mradm::binaural_internal::build_binaural_state(std::move(*data), 512);
        ok &= check(state != nullptr, "SOFA data reaches HRTF preparation");
        auto mismatch = load_sofa_dataset(root / filename, 96000);
        ok &= check(!mismatch && mismatch.error().code == mradm::ErrorCode::unsupported,
                    "offline sample-rate mismatch stays unsupported");
    }
    auto broken = load_sofa_dataset(root / "malformed.sofa", 0);
    ok &= check(!broken && broken.error().code == mradm::ErrorCode::io_error, "malformed SOFA is an I/O error");
    auto missing = load_sofa_dataset(root / "missing.sofa", 0);
    ok &= check(!missing && missing.error().code == mradm::ErrorCode::io_error, "missing SOFA is an I/O error");
#else
    auto disabled = load_sofa_dataset(root / "simple.sofa", 0);
    ok &= check(!disabled && disabled.error().code == mradm::ErrorCode::unsupported, "SOFA-off capability");
#endif
    return ok ? 0 : 1;
}
