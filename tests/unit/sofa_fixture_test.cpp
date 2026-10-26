#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "hrtf_state.h"

namespace {
bool check(bool value, std::string_view message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}

bool check_unicode_paths(const std::filesystem::path& root) {
    using mradm::binaural_internal::HrtfState;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("mradm-sofa-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto nested = directory / std::filesystem::path{u8"\u6d4b\u8bd5-\U0001f3a7"};
    const auto path = nested / std::filesystem::path{u8"hrtf-\U0001f3a7.sofa"};
    bool ok = true;
    try {
        std::filesystem::create_directories(nested);
        std::filesystem::copy_file(root / "simple.sofa", path);
        const auto prepared = HrtfState::prepare(path, 48000U, 512U);
        const auto live = HrtfState::prepare_live(path, 44100U);
#if MR_ADM_ENABLE_SOFA
        ok &= check(prepared.has_value() && live.has_value(), "Unicode SOFA paths load in both HRTF preparation flows");
        if (prepared && live) {
            const auto reference = HrtfState::prepare(root / "simple.sofa", 48000U, 512U);
            const auto reference_live = HrtfState::prepare_live(root / "simple.sofa", 44100U);
            if (!check(reference.has_value() && reference_live.has_value(), "reference SOFA preparation")) {
                ok = false;
            } else {
                const auto spectra = (*prepared)->filters.spectrum_snapshot();
                const auto expected = (*reference)->filters.spectrum_snapshot();
                const auto live_spectra = (*live)->filters.spectrum_snapshot();
                const auto expected_live = (*reference_live)->filters.spectrum_snapshot();
                ok &= check(spectra && expected && *spectra == *expected, "Unicode path preserves file HRTF spectra");
                ok &= check(live_spectra && expected_live && *live_spectra == *expected_live,
                            "Unicode path preserves resampled Scene HRTF spectra");
                ok &= check((*prepared)->dataset_name == (*reference)->dataset_name &&
                                (*live)->dataset_name == (*reference_live)->dataset_name,
                            "Unicode path preserves the SOFA listener name");
            }
        }
#else
        ok &= check(!prepared && prepared.error().code == mradm::ErrorCode::unsupported && !live &&
                        live.error().code == mradm::ErrorCode::unsupported,
                    "Unicode SOFA paths retain the SOFA-off error");
#endif
        const auto missing_path = nested / std::filesystem::path{u8"missing-\U0001f3a7.sofa"};
        const auto missing = HrtfState::prepare(missing_path, 48000U, 512U);
        const auto missing_utf8 = missing_path.u8string();
        ok &= check(!missing &&
                        missing.error().context == "path=" + std::string{missing_utf8.begin(), missing_utf8.end()},
                    "Unicode SOFA error context retains UTF-8 path bytes");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: Unicode SOFA preparation threw: " << error.what() << '\n';
        ok = false;
    }
    std::filesystem::remove_all(directory);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    // The file renderer's preparation (Rust, PCM consistency 4.7 A); input rate 0 accepts any rate.
    const auto load_sofa_dataset = [](const std::filesystem::path& path, std::uint32_t rate) {
        return mradm::binaural_internal::HrtfState::prepare(path, rate, 512U);
    };
    const std::filesystem::path root{argv[1]};
    bool ok = true;
#if MR_ADM_ENABLE_SOFA
    for (const auto* filename : {"simple.sofa", "general-compressed.sofa"}) {
        auto data = load_sofa_dataset(root / filename, 0);
        if (!check(data.has_value(), filename)) {
            return 1;
        }
        const auto& state = **data;
        ok &= check(state.num_dirs == 6 && state.hrir_len == 16, "SOFA dimensions");
        const int rate = std::string_view{filename} == "simple.sofa" ? 48000 : 44100;
        ok &= check(state.sample_rate == rate, "native sample rate is retained");
        ok &= check(state.dataset_name == "SOFA MacinRender synthetic", "SOFA listener name");
        for (std::size_t dir = 0; dir < 6U; ++dir) {
            ok &= check(state.hrtf_td[((dir * 2U) * 16U) + 2U] == static_cast<float>(dir + 1U) / 8.0F,
                        "left HRIR is not normalized");
            ok &= check(state.hrtf_td[(((dir * 2U) + 1U) * 16U) + 4U] == static_cast<float>(6U - dir) / 8.0F,
                        "right HRIR is not normalized");
        }
        ok &= check(state.fft_size == 1024 && state.overlap_len == 15, "SOFA data reaches HRTF preparation");
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
    ok &= check_unicode_paths(root);
    return ok ? 0 : 1;
}
