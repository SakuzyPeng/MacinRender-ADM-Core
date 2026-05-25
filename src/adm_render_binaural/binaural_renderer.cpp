#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ebur128.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
// clang-format off
// saf_utility_complex.h must precede saf_hrir.h: saf_hrir.h opens extern "C" and
// re-includes saf_utility_complex.h inside that block, causing std::complex<float>
// template instantiation in C linkage if the include guard hasn't fired yet.
#include <saf_utility_complex.h>
#include <saf_utility_fft.h>
#include <saf_hrir.h>
#include <saf_sofa_reader.h>
#include <saf_vbap.h>
// clang-format on

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

#include "render_common.h"

namespace mradm {

namespace {

// ── Constants ────────────────────────────────────────────────────────────────

constexpr uint64_t k_min_block_size = 1024U;
constexpr int k_n_ears = 2;
constexpr int k_n_azi = 361;  // (int)(360/1 + 0.5) + 1
constexpr int k_n_elev = 181; // (int)(180/1 + 0.5) + 1
constexpr std::size_t k_binaural_divergence_slots = 3U;
constexpr std::size_t k_binaural_center_slot = 1U;
constexpr std::size_t k_binaural_extent_slots = 17U;
constexpr std::size_t k_binaural_extent_center_slot = 0U;
constexpr std::size_t k_diffuse_delay_len = 32U;

// ── Helpers ───────────────────────────────────────────────────────────────────

// BS.2051 speaker label → (az_deg, el_deg).  ADM convention: +az = left.
// Returns NaN azimuth when the label is not in the table.
std::pair<float, float> label_to_polar(const std::string& label) {
    static const std::unordered_map<std::string, std::pair<float, float>> k_tab{
        {"M+000", {0.0F, 0.0F}},     {"M+030", {30.0F, 0.0F}},   {"M-030", {-30.0F, 0.0F}},
        {"M+060", {60.0F, 0.0F}},    {"M-060", {-60.0F, 0.0F}},  {"M+090", {90.0F, 0.0F}},
        {"M-090", {-90.0F, 0.0F}},   {"M+110", {110.0F, 0.0F}},  {"M-110", {-110.0F, 0.0F}},
        {"M+135", {135.0F, 0.0F}},   {"M-135", {-135.0F, 0.0F}}, {"M+180", {180.0F, 0.0F}},
        {"U+030", {30.0F, 30.0F}},   {"U-030", {-30.0F, 30.0F}}, {"U+000", {0.0F, 30.0F}},
        {"U+045", {45.0F, 30.0F}},   {"U-045", {-45.0F, 30.0F}}, {"U+110", {110.0F, 30.0F}},
        {"U-110", {-110.0F, 30.0F}}, {"U+135", {135.0F, 30.0F}}, {"U-135", {-135.0F, 30.0F}},
        {"U+180", {180.0F, 30.0F}},  {"T+000", {0.0F, 90.0F}},   {"B+045", {45.0F, -30.0F}},
        {"B-045", {-45.0F, -30.0F}},
    };
    auto it = k_tab.find(label);
    return it != k_tab.end() ? it->second : std::pair<float, float>{std::numeric_limits<float>::quiet_NaN(), 0.0F};
}

bool is_lfe_label(const std::string& label) {
    std::string upper(label.size(), '\0');
    std::ranges::transform(label, upper.begin(), [](const char ch) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    });
    return upper == "LF" || upper.find("LFE") != std::string::npos || upper.find("SUB") != std::string::npos ||
           upper.find("LOWFREQUENCY") != std::string::npos || upper.find("LOW_FREQUENCY") != std::string::npos;
}

int next_pow2_int(int value) {
    int out = 1;
    while (out < value) {
        out <<= 1U;
    }
    return out;
}

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] Vec3 add(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vec3 scale(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept {
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

[[nodiscard]] Vec3 normalize(Vec3 v) noexcept {
    const float n = std::hypot(v.x, v.y, v.z);
    if (n <= 1.0e-8F) {
        return {0.0F, 1.0F, 0.0F};
    }
    return {v.x / n, v.y / n, v.z / n};
}

// Grid index into the pre-computed VBAP table (az_res=1°, el_res=1°).
int vbap_grid_idx(float az_deg, float el_deg) {
    float az_norm = fmodf(az_deg + 180.0F, 360.0F);
    if (az_norm < 0.0F) {
        az_norm += 360.0F;
    }
    const int az_idx = std::min(static_cast<int>(std::lround(az_norm)), k_n_azi - 1);
    const int el_idx = std::clamp(static_cast<int>(std::lround(el_deg + 90.0F)), 0, k_n_elev - 1);
    return (el_idx * k_n_azi) + az_idx;
}

[[nodiscard]] Vec3 direction_from_position(const SceneBlockPosition& pos) noexcept {
    const auto polar = scene_position_to_polar(pos);
    const double az = static_cast<double>(polar.azimuth) * (std::numbers::pi_v<double> / 180.0);
    const double el = static_cast<double>(polar.elevation) * (std::numbers::pi_v<double> / 180.0);
    const double cos_el = std::cos(el);
    return normalize({
        static_cast<float>(-std::sin(az) * cos_el),
        static_cast<float>(std::cos(az) * cos_el),
        static_cast<float>(std::sin(el)),
    });
}

[[nodiscard]] float distance_from_position(const SceneBlockPosition& pos) noexcept {
    return pos.cartesian ? std::hypot(pos.x, pos.y, pos.z) : pos.distance;
}

[[nodiscard]] std::pair<float, float> polar_from_direction(Vec3 dir) noexcept {
    const Vec3 n = normalize(dir);
    const double az =
        std::atan2(static_cast<double>(-n.x), static_cast<double>(n.y)) * (180.0 / std::numbers::pi_v<double>);
    const double el =
        std::atan2(static_cast<double>(n.z), std::hypot(static_cast<double>(n.x), static_cast<double>(n.y))) *
        (180.0 / std::numbers::pi_v<double>);
    return {static_cast<float>(az), static_cast<float>(el)};
}

#ifdef SAF_ENABLE_SOFA_READER_MODULE
// SOFA Cartesian (front=+X, left=+Y, up=+Z) → polar az/el in degrees.
std::pair<float, float> sofa_cart_to_polar(float x, float y, float z) {
    const double az = std::atan2(static_cast<double>(y), static_cast<double>(x)) * (180.0 / std::numbers::pi_v<double>);
    const auto cx = static_cast<double>(x);
    const auto cy = static_cast<double>(y);
    const double el =
        std::atan2(static_cast<double>(z), std::sqrt((cx * cx) + (cy * cy))) * (180.0 / std::numbers::pi_v<double>);
    return {static_cast<float>(az), static_cast<float>(el)};
}

[[nodiscard]] bool string_is(const char* value, std::string_view expected) {
    return value != nullptr && expected == std::string_view{value};
}

[[nodiscard]] bool contains_token(const char* value, std::string_view token) {
    return value != nullptr && std::string_view{value}.find(token) != std::string_view::npos;
}

[[nodiscard]] std::string sofa_error_name(int code) {
    switch (code) {
    case SAF_SOFA_OK:
        return "OK";
    case SAF_SOFA_ERROR_INVALID_FILE_OR_FILE_PATH:
        return "invalid file or path";
    case SAF_SOFA_ERROR_DIMENSIONS_UNEXPECTED:
        return "unexpected dimensions";
    case SAF_SOFA_ERROR_FORMAT_UNEXPECTED:
        return "unexpected format";
    case SAF_SOFA_ERROR_NETCDF_IN_USE:
        return "NetCDF reader already in use";
    default:
        return fmt::format("unknown ({})", code);
    }
}
#endif

// ── HRTF dataset and pre-computed state ───────────────────────────────────────

struct HrtfDataset {
    std::string name;
    int sample_rate{0};
    int hrir_len{0};
    int num_dirs{0};
    std::vector<float> dirs_deg; // FLAT: [num_dirs × 2] az/el in degrees
    std::vector<float> hrirs;    // FLAT: [num_dirs × 2 × hrir_len]
};

HrtfDataset built_in_kemar_dataset() {
    HrtfDataset ds;
    ds.name = "built-in KEMAR";
    ds.sample_rate = __default_hrir_fs;
    ds.hrir_len = __default_hrir_len;
    ds.num_dirs = __default_N_hrir_dirs;
    ds.dirs_deg.assign(&__default_hrir_dirs_deg[0][0],
                       &__default_hrir_dirs_deg[0][0] + (static_cast<std::ptrdiff_t>(ds.num_dirs) * 2));
    ds.hrirs.assign(&__default_hrirs[0][0][0],
                    &__default_hrirs[0][0][0] + (static_cast<std::ptrdiff_t>(ds.num_dirs) * k_n_ears * ds.hrir_len));
    return ds;
}

#ifdef SAF_ENABLE_SOFA_READER_MODULE
Result<HrtfDataset> load_sofa_dataset(const std::filesystem::path& path, uint32_t input_sample_rate) {
    std::string sofa_path = path.string();
    saf_sofa_container sofa{};
    const auto sofa_err = saf_sofa_open(&sofa, sofa_path.data(), SAF_SOFA_READER_OPTION_LIBMYSOFA);
    if (sofa_err != SAF_SOFA_OK) {
        return make_error(
            ErrorCode::io_error, fmt::format("SOFA load failed: {}", sofa_error_name(sofa_err)), "path=" + sofa_path);
    }

    class SofaGuard {
      public:
        explicit SofaGuard(saf_sofa_container* sofa) : sofa_(sofa) {}
        SofaGuard(const SofaGuard&) = delete;
        SofaGuard& operator=(const SofaGuard&) = delete;
        SofaGuard(SofaGuard&&) = delete;
        SofaGuard& operator=(SofaGuard&&) = delete;
        ~SofaGuard() { saf_sofa_close(sofa_); }

      private:
        saf_sofa_container* sofa_;
    } sofa_guard{&sofa};

    if (!string_is(sofa.SOFAConventions, "SimpleFreeFieldHRIR") && !string_is(sofa.SOFAConventions, "GeneralFIR")) {
        return make_error(ErrorCode::unsupported,
                          "SOFA: only SimpleFreeFieldHRIR and GeneralFIR conventions are supported",
                          "path=" + sofa_path);
    }
    if (!string_is(sofa.DataType, "FIR")) {
        return make_error(ErrorCode::unsupported, "SOFA: only FIR data is supported", "path=" + sofa_path);
    }
    if (sofa.nReceivers != k_n_ears) {
        return make_error(ErrorCode::unsupported,
                          fmt::format("SOFA: expected 2 receivers, got {}", sofa.nReceivers),
                          "path=" + sofa_path);
    }
    if (std::lround(sofa.DataSamplingRate) != static_cast<long>(input_sample_rate)) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("SOFA: sample rate {} Hz does not match input {} Hz", sofa.DataSamplingRate, input_sample_rate),
            "path=" + sofa_path);
    }
    if (sofa.DataLengthIR <= 0 || sofa.nSources < 4 || sofa.DataIR == nullptr || sofa.SourcePosition == nullptr) {
        return make_error(ErrorCode::unsupported, "SOFA: missing usable FIR directions", "path=" + sofa_path);
    }

    const bool spherical = string_is(sofa.SourcePositionType, "spherical");
    const bool cartesian = string_is(sofa.SourcePositionType, "cartesian");
    if (!spherical && !cartesian) {
        return make_error(ErrorCode::unsupported, "SOFA: unsupported SourcePosition Type", "path=" + sofa_path);
    }
    if (spherical &&
        (!contains_token(sofa.SourcePositionUnits, "degree") || !contains_token(sofa.SourcePositionUnits, "met"))) {
        return make_error(
            ErrorCode::unsupported, "SOFA: spherical SourcePosition units must be degrees/metres", "path=" + sofa_path);
    }
    if (cartesian && !contains_token(sofa.SourcePositionUnits, "met")) {
        return make_error(
            ErrorCode::unsupported, "SOFA: cartesian SourcePosition units must be metres", "path=" + sofa_path);
    }

    HrtfDataset ds;
    ds.name = sofa.ListenerShortName != nullptr ? fmt::format("SOFA {}", sofa.ListenerShortName)
                                                : fmt::format("SOFA {}", path.filename().string());
    ds.sample_rate = static_cast<int>(std::lround(sofa.DataSamplingRate));
    ds.hrir_len = sofa.DataLengthIR;
    ds.num_dirs = sofa.nSources;
    ds.hrirs.assign(sofa.DataIR, sofa.DataIR + (static_cast<std::ptrdiff_t>(ds.num_dirs) * k_n_ears * ds.hrir_len));
    ds.dirs_deg.resize(static_cast<std::size_t>(ds.num_dirs) * 2U);
    for (int i = 0; i < ds.num_dirs; ++i) {
        const float a = sofa.SourcePosition[(static_cast<std::size_t>(i) * 3U) + 0U];
        const float b = sofa.SourcePosition[(static_cast<std::size_t>(i) * 3U) + 1U];
        const float c = sofa.SourcePosition[(static_cast<std::size_t>(i) * 3U) + 2U];
        auto [az, el] = spherical ? std::pair<float, float>{a, b} : sofa_cart_to_polar(a, b, c);
        ds.dirs_deg[(static_cast<std::size_t>(i) * 2U) + 0U] = az;
        ds.dirs_deg[(static_cast<std::size_t>(i) * 2U) + 1U] = el;
    }
    return ds;
}
#else
Result<HrtfDataset> load_sofa_dataset(const std::filesystem::path& path, uint32_t input_sample_rate) {
    (void) input_sample_rate;
    return make_error(ErrorCode::unsupported,
                      "SOFA loading is disabled in this build (MR_ADM_ENABLE_SOFA=OFF)",
                      "path=" + path.string());
}
#endif

Result<HrtfDataset> load_hrtf_dataset(const RenderPlan& plan) {
    if (plan.sofa_path.has_value()) {
        return load_sofa_dataset(*plan.sofa_path, plan.scene.info.sample_rate);
    }
    return built_in_kemar_dataset();
}

// NOLINTBEGIN(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)
struct BinauralState {
    int num_dirs{0};
    int hrir_len{0};
    int fft_size{0};
    int n_bands{0};
    int overlap_len{0};
    std::string dataset_name;
    // HRTFs in frequency domain; FLAT: [n_bands × k_n_ears × num_dirs]
    std::vector<float_complex> hrtf_fd;
    // Compressed VBAP table: amplitude-normalised gains + direction indices per grid point.
    std::vector<float> vbap_gains; // FLAT: [N_gtable × 3]
    std::vector<int> vbap_dirs;    // FLAT: [N_gtable × 3]

    BinauralState() = default;
    BinauralState(const BinauralState&) = delete;
    BinauralState& operator=(const BinauralState&) = delete;
};
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)

// Build BinauralState once.  Returns nullptr on VBAP triangulation failure.
std::unique_ptr<BinauralState> build_binaural_state(HrtfDataset dataset, uint64_t block_size) {
    auto bs = std::make_unique<BinauralState>();
    bs->num_dirs = dataset.num_dirs;
    bs->hrir_len = dataset.hrir_len;
    bs->fft_size = next_pow2_int(static_cast<int>(block_size) + dataset.hrir_len - 1);
    bs->n_bands = (bs->fft_size / 2) + 1;
    bs->overlap_len = dataset.hrir_len - 1;
    bs->dataset_name = std::move(dataset.name);

    // Convert HRIRs to frequency-domain HRTFs.
    bs->hrtf_fd.resize(static_cast<std::size_t>(bs->n_bands) * k_n_ears * static_cast<std::size_t>(bs->num_dirs));
    HRIRs2HRTFs(dataset.hrirs.data(), bs->num_dirs, bs->hrir_len, bs->fft_size, bs->hrtf_fd.data());

    // Build VBAP gain table for the HRTF measurement directions as "loudspeakers".
    float* gtable_full = nullptr;
    int n_gtable = 0;
    int n_triangles = 0;
    generateVBAPgainTable3D(dataset.dirs_deg.data(),
                            bs->num_dirs,
                            /*az_res=*/1,
                            /*el_res=*/1,
                            /*omitLargeTriangles=*/1,
                            /*enableDummies=*/1,
                            /*spread=*/0.0F,
                            &gtable_full,
                            &n_gtable,
                            &n_triangles);
    if (gtable_full == nullptr) {
        return nullptr; // triangulation failed
    }

    // Compress to 3-per-point amplitude-normalised table (sum of gains = 1).
    bs->vbap_gains.resize(static_cast<std::size_t>(n_gtable) * 3);
    bs->vbap_dirs.resize(static_cast<std::size_t>(n_gtable) * 3);
    compressVBAPgainTable3D(gtable_full, n_gtable, bs->num_dirs, bs->vbap_gains.data(), bs->vbap_dirs.data());
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    std::free(gtable_full);

    return bs;
}

// Interpolate HRTF at (az_deg, el_deg) using the compressed VBAP table.
// Returns flat [n_bands × k_n_ears] complex array (L = ear 0, R = ear 1).
std::vector<float_complex> hrtf_for_dir(const BinauralState& bs, float az_deg, float el_deg) {
    const auto g = static_cast<std::size_t>(vbap_grid_idx(az_deg, el_deg));
    const auto gbase = g * 3U;
    const float w0 = bs.vbap_gains[gbase + 0U];
    const float w1 = bs.vbap_gains[gbase + 1U];
    const float w2 = bs.vbap_gains[gbase + 2U];
    const int d0 = bs.vbap_dirs[gbase + 0U];
    const int d1 = bs.vbap_dirs[gbase + 1U];
    const int d2 = bs.vbap_dirs[gbase + 2U];

    std::vector<float_complex> out(static_cast<std::size_t>(bs.n_bands) * k_n_ears);
    for (int band = 0; band < bs.n_bands; ++band) {
        for (int ear = 0; ear < k_n_ears; ++ear) {
            const auto base = (static_cast<std::ptrdiff_t>(band) * k_n_ears * bs.num_dirs) +
                              (static_cast<std::ptrdiff_t>(ear) * bs.num_dirs);
            out[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)] =
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d0)], w0) +
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d1)], w1) +
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d2)], w2);
        }
    }
    return out;
}

// ── Per-channel OLA convolution state ─────────────────────────────────────────

class OLAState {
  public:
    explicit OLAState(int overlap_len)
        : overlap_l(static_cast<std::size_t>(overlap_len), 0.0F),
          overlap_r(static_cast<std::size_t>(overlap_len), 0.0F) {}

    [[nodiscard]] std::vector<float>& left() noexcept { return overlap_l; }
    [[nodiscard]] std::vector<float>& right() noexcept { return overlap_r; }

  private:
    std::vector<float> overlap_l;
    std::vector<float> overlap_r;
};

struct DiffuseDelayState {
    std::array<float, k_diffuse_delay_len> delay_line{};
    std::size_t write_pos{0};
};

void decorrelate_diffuse_mono(DiffuseDelayState& state, const float* in, uint64_t frames_now, float* out) {
    constexpr std::array<std::size_t, 8U> k_offsets{3U, 7U, 11U, 17U, 19U, 23U, 29U, 31U};
    constexpr std::array<float, 8U> k_polarity{1.0F, -1.0F, 1.0F, 1.0F, -1.0F, 1.0F, -1.0F, -1.0F};
    constexpr float k_norm = 0.35355339F; // 1/sqrt(8)

    for (std::size_t f = 0; f < static_cast<std::size_t>(frames_now); ++f) {
        state.delay_line.at(state.write_pos) = in[f];
        float sum = 0.0F;
        for (std::size_t i = 0; i < k_offsets.size(); ++i) {
            const std::size_t read_pos =
                (state.write_pos + k_diffuse_delay_len - k_offsets.at(i)) % k_diffuse_delay_len;
            sum += state.delay_line.at(read_pos) * k_polarity.at(i);
        }
        out[f] = sum * k_norm;
        state.write_pos = (state.write_pos + 1U) % k_diffuse_delay_len;
    }
}

// OLA convolution: convolve src[frames_now] with hrtf[n_bands × k_n_ears],
// accumulate into l_out/r_out (both [frames_now]), update state.overlap.
// Uses saf_rfft (KissFFT backend).
// NOLINTNEXTLINE(readability-function-size)
void convolve_and_accumulate(void* hfft,
                             const BinauralState& bs,
                             const float* src,
                             uint64_t frames_now,
                             float gain,
                             const std::vector<float_complex>& hrtf,
                             OLAState& state,
                             float* l_out,
                             float* r_out) {
    const auto fn = static_cast<std::size_t>(frames_now);

    std::vector<float> buf(static_cast<std::size_t>(bs.fft_size), 0.0F);
    std::vector<float_complex> src_fd(static_cast<std::size_t>(bs.n_bands));
    std::vector<float_complex> out_fd(static_cast<std::size_t>(bs.n_bands));
    std::vector<float> y(static_cast<std::size_t>(bs.fft_size));

    // Zero-pad source block into FFT buffer and transform.
    std::copy_n(src, fn, buf.begin());
    saf_rfft_forward(hfft, buf.data(), src_fd.data());

    for (int ear = 0; ear < k_n_ears; ++ear) {
        // Frequency-domain multiply: src_fd × hrtf[:][ear].
        for (int band = 0; band < bs.n_bands; ++band) {
            out_fd[static_cast<std::size_t>(band)] =
                gain * src_fd[static_cast<std::size_t>(band)] *
                hrtf[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)];
        }
        saf_rfft_backward(hfft, out_fd.data(), y.data());

        auto& overlap_vec = (ear == 0) ? state.left() : state.right();
        float* overlap = overlap_vec.data();
        float* dst = (ear == 0) ? l_out : r_out;
        const auto overlap_len = static_cast<std::size_t>(bs.overlap_len);

        // Overlap-add: accumulated output = y[0..fn-1] + saved overlap.
        for (std::size_t f = 0; f < fn; ++f) {
            dst[f] += y[f] + (f < overlap_len ? overlap[f] : 0.0F);
        }
        // Save new overlap: y[fn..fn+overlap_len-1] plus any unemitted residual from the
        // old overlap.  When fn < overlap_len, overlap[fn..overlap_len-1] was never written
        // to dst; those samples belong at output positions [fn..overlap_len-1] relative to
        // this segment and must be carried forward.  Processing i in ascending order is safe:
        // the read index (fn+i) exceeds the write index (i) for all i < fn, so no aliasing.
        for (std::size_t i = 0; i < overlap_len; ++i) {
            const float residual = (fn + i < overlap_len) ? overlap[fn + i] : 0.0F;
            overlap[i] = y[fn + i] + residual;
        }
    }
}

// Advance a source's FIR overlap through a silent interval.  This emits the tail
// from a previous active segment at the correct absolute time instead of carrying
// it forward to the next non-silent ADM block.
void advance_silence(OLAState& state, uint64_t frames_now, float* l_out, float* r_out) {
    const auto fn = static_cast<std::size_t>(frames_now);
    auto& overlap_l = state.left();
    auto& overlap_r = state.right();
    const auto overlap_len = overlap_l.size();
    const auto emit = std::min(fn, overlap_len);
    for (std::size_t f = 0; f < emit; ++f) {
        l_out[f] += overlap_l[f];
        r_out[f] += overlap_r[f];
    }

    if (fn >= overlap_len) {
        std::ranges::fill(overlap_l, 0.0F);
        std::ranges::fill(overlap_r, 0.0F);
        return;
    }

    const auto remain = overlap_len - fn;
    std::move(overlap_l.begin() + static_cast<std::ptrdiff_t>(fn), overlap_l.end(), overlap_l.begin());
    std::move(overlap_r.begin() + static_cast<std::ptrdiff_t>(fn), overlap_r.end(), overlap_r.begin());
    std::fill(overlap_l.begin() + static_cast<std::ptrdiff_t>(remain), overlap_l.end(), 0.0F);
    std::fill(overlap_r.begin() + static_cast<std::ptrdiff_t>(remain), overlap_r.end(), 0.0F);
}

// ── Source descriptor ─────────────────────────────────────────────────────────

// One renderable source extracted from the ADM scene.
struct BinauralSource {
    uint16_t channel_index{0};
    float gain{1.0F}; // object-level gain
    bool bypass_lfe{false};
    bool smoothable_object{false};
    bool diffuse_bus{false};
    struct Block {
        float az{0.0F};
        float el{0.0F};
        float block_gain{1.0F};
        uint64_t start_sample{0};
        uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
        bool jump_position{false};
        std::optional<uint64_t> interp_length_samples;
    };
    std::vector<Block> blocks;
};

// Binaural has no physical speaker layout, but channelLock still needs a
// deterministic reproduction reference. Use the conventional ±30° stereo pair.
[[nodiscard]] std::vector<SceneOutputSpeaker> binaural_lock_speakers() {
    return {
        {30.0F, 0.0F, false},
        {-30.0F, 0.0F, false},
    };
}

// Resolve az/el from a preprocessed SceneObjectBlock.
std::pair<float, float> block_position(const SceneObjectBlock& blk) {
    const SceneBlockPosition pos = scene_position_to_polar(blk.position);
    return {pos.azimuth, pos.elevation};
}

struct ExtentSource {
    float az{0.0F};
    float el{0.0F};
    float gain{0.0F};
    std::size_t slot{k_binaural_extent_center_slot};
};

[[nodiscard]] std::vector<ExtentSource> expand_binaural_extent(const SceneObjectBlock& block, float source_gain) {
    const float distance = distance_from_position(block.position);
    const float spread_scale = std::clamp(1.0F / std::max(0.4F, distance), 0.5F, 2.5F);
    const float depth_radius = std::max(0.0F, block.depth) * 20.0F * spread_scale;
    const float width_radius = (std::max(0.0F, block.width) * 60.0F * spread_scale) + depth_radius;
    const float height_radius = (std::max(0.0F, block.height) * 45.0F * spread_scale) + depth_radius;
    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }

    struct DiskSample {
        float x{0.0F};
        float y{0.0F};
        float weight{0.0F};
    };
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    constexpr float k_outer_weight = 1.0F / 12.0F; // outer ring total = 2/3
    constexpr float k_inner_weight = 1.0F / 24.0F; // inner ring total = 1/3
    constexpr std::array<DiskSample, k_binaural_extent_slots> k_samples = {{
        {0.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, k_outer_weight},
        {-1.0F, 0.0F, k_outer_weight},
        {0.0F, 1.0F, k_outer_weight},
        {0.0F, -1.0F, k_outer_weight},
        {0.70710678F, 0.70710678F, k_outer_weight},
        {-0.70710678F, 0.70710678F, k_outer_weight},
        {0.70710678F, -0.70710678F, k_outer_weight},
        {-0.70710678F, -0.70710678F, k_outer_weight},
        {0.5F, 0.0F, k_inner_weight},
        {-0.5F, 0.0F, k_inner_weight},
        {0.0F, 0.5F, k_inner_weight},
        {0.0F, -0.5F, k_inner_weight},
        {0.35355339F, 0.35355339F, k_inner_weight},
        {-0.35355339F, 0.35355339F, k_inner_weight},
        {0.35355339F, -0.35355339F, k_inner_weight},
        {-0.35355339F, -0.35355339F, k_inner_weight},
    }};

    const Vec3 center = direction_from_position(block.position);
    Vec3 horizontal = cross({0.0F, 0.0F, 1.0F}, center);
    if (std::hypot(horizontal.x, horizontal.y, horizontal.z) < 1.0e-4F) {
        horizontal = {1.0F, 0.0F, 0.0F};
    } else {
        horizontal = normalize(horizontal);
    }
    const Vec3 vertical = normalize(cross(center, horizontal));

    std::vector<ExtentSource> sources;
    sources.reserve(k_binaural_extent_slots - 1U);
    std::size_t slot = 0;
    for (const auto& sample : k_samples) {
        if (sample.weight <= 0.0F) {
            ++slot;
            continue;
        }
        const float h = std::tan(sample.x * width_radius * k_deg2rad);
        const float v = std::tan(sample.y * height_radius * k_deg2rad);
        auto [az, el] = polar_from_direction(normalize(add(add(center, scale(horizontal, h)), scale(vertical, v))));
        sources.push_back({az, el, source_gain * sample.weight, slot});
        ++slot;
    }
    return sources;
}

struct BinauralSourceBank {
    std::array<BinauralSource, k_binaural_divergence_slots * k_binaural_extent_slots> direct;
    std::array<BinauralSource, k_binaural_divergence_slots * k_binaural_extent_slots> diffuse;
};

void init_source_bank(BinauralSourceBank& bank, uint16_t channel_index, float object_gain) {
    for (auto& src : bank.direct) {
        src.channel_index = channel_index;
        src.gain = object_gain;
        src.smoothable_object = true;
        src.diffuse_bus = false;
    }
    for (auto& src : bank.diffuse) {
        src.channel_index = channel_index;
        src.gain = object_gain;
        src.smoothable_object = true;
        src.diffuse_bus = true;
    }
}

void append_source_bank(std::span<BinauralSource> bank, std::vector<BinauralSource>& srcs) {
    for (auto& src : bank) {
        if (!src.blocks.empty()) {
            std::ranges::sort(src.blocks, {}, &BinauralSource::Block::start_sample);
            srcs.push_back(std::move(src));
        }
    }
}

void append_object_track_sources(const SceneObject& obj,
                                 const SceneTrackRef& track,
                                 uint16_t channel_index,
                                 const std::vector<SceneOutputSpeaker>& lock_speakers,
                                 bool& screen_ref_warned,
                                 LogSink& logs,
                                 std::vector<BinauralSource>& srcs) {
    BinauralSourceBank track_sources;
    init_source_bank(track_sources, channel_index, obj.gain);

    for (const auto& blk : track.blocks) {
        const auto prepared =
            render_common::prepare_object_block(blk, obj, lock_speakers, logs, "binaural", screen_ref_warned);
        for (std::size_t source_index = 0; source_index < prepared.sources.size(); ++source_index) {
            const auto& source_block = prepared.sources[source_index];
            const std::size_t divergence_slot =
                prepared.sources.size() == k_binaural_divergence_slots ? source_index : k_binaural_center_slot;
            const float diffuse = std::clamp(source_block.diffuse, 0.0F, 1.0F);
            const float direct_scale = std::sqrt(1.0F - diffuse);
            const float diffuse_scale = std::sqrt(diffuse);
            if (direct_scale > 1.0e-4F) {
                for (const auto& extent_source :
                     expand_binaural_extent(source_block, source_block.gain * direct_scale)) {
                    const std::size_t slot = (divergence_slot * k_binaural_extent_slots) + extent_source.slot;
                    track_sources.direct.at(slot).blocks.push_back({extent_source.az,
                                                                    extent_source.el,
                                                                    extent_source.gain,
                                                                    prepared.start_sample,
                                                                    prepared.end_sample,
                                                                    prepared.jump_position,
                                                                    prepared.interp_length_samples});
                }
            }
            if (diffuse_scale <= 1.0e-4F) {
                continue;
            }
            for (const auto& extent_source : expand_binaural_extent(source_block, source_block.gain * diffuse_scale)) {
                const std::size_t slot = (divergence_slot * k_binaural_extent_slots) + extent_source.slot;
                track_sources.diffuse.at(slot).blocks.push_back({extent_source.az,
                                                                 extent_source.el,
                                                                 extent_source.gain,
                                                                 prepared.start_sample,
                                                                 prepared.end_sample,
                                                                 prepared.jump_position,
                                                                 prepared.interp_length_samples});
            }
        }
    }
    append_source_bank(track_sources.direct, srcs);
    append_source_bank(track_sources.diffuse, srcs);
}

// Build source list from scene.
std::vector<BinauralSource> build_sources(const AdmScene& scene, LogSink& logs) {
    std::vector<BinauralSource> srcs;
    const auto lock_speakers = binaural_lock_speakers();
    bool screen_ref_warned{false};

    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value()) {
                continue;
            }
            const auto channel_index = *track.channel_index;

            // Objects-type blocks.
            if (!track.blocks.empty()) {
                append_object_track_sources(obj, track, channel_index, lock_speakers, screen_ref_warned, logs, srcs);
            }

            // DirectSpeakers-type blocks.
            for (const auto& ds : track.ds_blocks) {
                const bool is_lfe =
                    ds.low_pass_hz.has_value() ||
                    std::ranges::any_of(ds.speaker_labels, [](const auto& label) { return is_lfe_label(label); });
                if (is_lfe) {
                    BinauralSource src;
                    src.channel_index = channel_index;
                    src.gain = obj.gain;
                    src.bypass_lfe = true;
                    src.blocks.push_back({0.0F,
                                          0.0F,
                                          ds.gain,
                                          ds.start_sample,
                                          std::min(ds.end_sample, obj.end_sample),
                                          true,
                                          std::nullopt});
                    srcs.push_back(std::move(src));
                    continue;
                }
                float az = 0.0F;
                float el = 0.0F;
                bool found = false;
                if (ds.has_position) {
                    az = ds.azimuth;
                    el = ds.elevation;
                    found = true;
                } else {
                    for (const auto& lbl : ds.speaker_labels) {
                        auto [la, le] = label_to_polar(lbl);
                        if (!std::isnan(la)) {
                            az = la;
                            el = le;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    logs.log(
                        LogLevel::warning,
                        "binaural",
                        fmt::format("DirectSpeakers channel {} has no resolvable position, skipping", channel_index));
                    continue;
                }
                BinauralSource src;
                src.channel_index = channel_index;
                src.gain = obj.gain;
                src.blocks.push_back(
                    {az, el, ds.gain, ds.start_sample, std::min(ds.end_sample, obj.end_sample), true, std::nullopt});
                srcs.push_back(std::move(src));
            }
        }
    }

    if (!scene.hoa_tracks.empty()) {
        logs.log(
            LogLevel::warning, "binaural", "HOA tracks are not supported by the binaural renderer and will be skipped");
    }
    return srcs;
}

// First block that has not ended before an absolute frame position.
std::size_t first_relevant_block(const BinauralSource& src, uint64_t frame) {
    std::size_t idx = 0;
    while (idx < src.blocks.size() && src.blocks[idx].end_sample <= frame) {
        ++idx;
    }
    return idx;
}

const BinauralSource::Block* first_overlapping_block(const BinauralSource& src, uint64_t start, uint64_t end) {
    const auto it = std::ranges::find_if(
        src.blocks, [start, end](const auto& block) { return block.end_sample > start && block.start_sample < end; });
    return it == src.blocks.end() ? nullptr : &*it;
}

const BinauralSource::Block* last_overlapping_block(const BinauralSource& src, uint64_t start, uint64_t end) {
    auto blocks_reversed = std::views::reverse(src.blocks);
    const auto it = std::ranges::find_if(blocks_reversed, [start, end](const auto& block) {
        return block.end_sample > start && block.start_sample < end;
    });
    return it == blocks_reversed.end() ? nullptr : std::addressof(*it);
}

void copy_windowed_object_input(const BinauralSource& src,
                                const float* input,
                                uint16_t num_in_ch,
                                uint64_t chunk_start,
                                uint64_t frames_now,
                                float* out) {
    std::ranges::fill_n(out, static_cast<std::ptrdiff_t>(frames_now), 0.0F);
    std::size_t bi = first_relevant_block(src, chunk_start);
    for (std::size_t f = 0; f < frames_now; ++f) {
        const uint64_t abs_frame = chunk_start + f;
        while (bi < src.blocks.size() && src.blocks[bi].end_sample <= abs_frame) {
            ++bi;
        }
        if (bi < src.blocks.size() && src.blocks[bi].start_sample <= abs_frame) {
            const uint16_t ic = src.channel_index;
            out[f] = (ic < num_in_ch) ? input[(f * num_in_ch) + ic] : 0.0F;
        }
    }
}

// NOLINTNEXTLINE(readability-function-size)
void convolve_crossfaded_object_block(void* hfft,
                                      const BinauralState& bs,
                                      const float* src,
                                      uint64_t frames_now,
                                      float start_gain,
                                      float end_gain,
                                      const std::vector<float_complex>& start_hrtf,
                                      const std::vector<float_complex>& end_hrtf,
                                      OLAState& state,
                                      float* l_out,
                                      float* r_out) {
    OLAState start_state = state;
    OLAState end_state = state;
    const auto fn = static_cast<std::size_t>(frames_now);
    std::vector<float> l_start(fn, 0.0F);
    std::vector<float> r_start(fn, 0.0F);
    std::vector<float> l_end(fn, 0.0F);
    std::vector<float> r_end(fn, 0.0F);

    convolve_and_accumulate(
        hfft, bs, src, frames_now, start_gain, start_hrtf, start_state, l_start.data(), r_start.data());
    convolve_and_accumulate(hfft, bs, src, frames_now, end_gain, end_hrtf, end_state, l_end.data(), r_end.data());

    for (std::size_t f = 0; f < fn; ++f) {
        const float alpha = fn > 1U ? static_cast<float>(f) / static_cast<float>(fn - 1U) : 0.0F;
        l_out[f] += (l_start[f] * (1.0F - alpha)) + (l_end[f] * alpha);
        r_out[f] += (r_start[f] * (1.0F - alpha)) + (r_end[f] * alpha);
    }
    state = std::move(end_state);
}

// ── Renderer ──────────────────────────────────────────────────────────────────

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<RenderMetrics> render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) override;
};

CapabilityReport BinauralRenderer::capabilities() const {
    return binaural_capabilities();
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> BinauralRenderer::render(const RenderPlan& plan, ProgressSink& progress, LogSink& logs) {
    const auto& info = plan.scene.info;

    if (info.sample_rate != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                          "input=" + plan.input_path);
    }

    const uint64_t render_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);

    // One-time setup: load HRIR dataset, then build HRTF table + VBAP grid.
    progress.on_progress({RenderStage::planning, 0.1, "building HRTF table"});
    auto dataset_res = load_hrtf_dataset(plan);
    if (!dataset_res) {
        return tl::unexpected{dataset_res.error()};
    }
    auto bs = build_binaural_state(std::move(*dataset_res), render_block_size);
    if (!bs) {
        return make_error(ErrorCode::internal_error, "binaural: VBAP triangulation of HRTF directions failed", {});
    }
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("HRTF source: {} ({} dirs, {} taps @ {} Hz)",
                         bs->dataset_name,
                         bs->num_dirs,
                         bs->hrir_len,
                         info.sample_rate));

    const auto sources = build_sources(plan.scene, logs);
    if (sources.empty()) {
        logs.log(LogLevel::warning, "binaural", "no renderable tracks found, writing silence");
    }
    logs.log(LogLevel::info, "binaural", fmt::format("{} source(s) to render", sources.size()));

    // Open I/O.
    auto reader = bw64::readFile(plan.input_path);
    auto writer_res = audio::WriterHandle::open(plan.output_path, 2U, info.sample_rate, "binaural");
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    // Inline loudness + true-peak measurement.
    struct EburFree {
        void operator()(ebur128_state* s) const noexcept { ebur128_destroy(&s); }
    };
    using EburPtr = std::unique_ptr<ebur128_state, EburFree>;
    EburPtr lufs_st{
        ebur128_init(2U, static_cast<unsigned long>(info.sample_rate), EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK)};

    // One FFT handle (KissFFT, thread-safe to use per render call).
    void* hfft = nullptr;
    saf_rfft_create(&hfft, bs->fft_size);
    class FftGuard {
      public:
        explicit FftGuard(void** handle) : handle_(handle) {}
        FftGuard(const FftGuard&) = delete;
        FftGuard& operator=(const FftGuard&) = delete;
        FftGuard(FftGuard&&) = delete;
        FftGuard& operator=(FftGuard&&) = delete;
        ~FftGuard() { saf_rfft_destroy(handle_); }

      private:
        void** handle_;
    } fft_guard{&hfft};

    // Per-source OLA state.
    std::vector<OLAState> ola;
    ola.reserve(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        ola.emplace_back(bs->overlap_len);
    }
    std::vector<DiffuseDelayState> diffuse_delay(sources.size());

    const uint64_t num_frames = info.num_frames;
    const uint16_t num_in_ch = info.num_channels;

    std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * render_block_size);
    std::vector<float> out_block(2U * render_block_size); // interleaved L/R
    uint64_t frames_done = 0;

    progress.on_progress({RenderStage::rendering, 0.2, "rendering"});

    while (frames_done < num_frames) {
        const uint64_t frames_now = std::min(render_block_size, num_frames - frames_done);
        const auto fn = static_cast<std::size_t>(frames_now);

        reader->read(in_block.data(), frames_now);
        std::ranges::fill(out_block, 0.0F);

        // Scratch L/R accumulation buffers (non-interleaved for SIMD friendliness).
        std::vector<float> l_buf(fn, 0.0F);
        std::vector<float> r_buf(fn, 0.0F);
        // De-interleaved input scratch for one channel.
        std::vector<float> ch_in(fn);
        std::vector<float> diffuse_in(fn);

        for (std::size_t si = 0; si < sources.size(); ++si) {
            const auto& src = sources[si];
            if (src.blocks.empty()) {
                continue;
            }

            const uint64_t chunk_start = frames_done;
            const uint64_t chunk_end = frames_done + frames_now;

            if (plan.object_smoothing_frames > 0 && src.smoothable_object && !src.bypass_lfe) {
                const auto* start_block = first_overlapping_block(src, chunk_start, chunk_end);
                const auto* end_block = last_overlapping_block(src, chunk_start, chunk_end);
                if (start_block != nullptr && end_block != nullptr && start_block != end_block &&
                    !end_block->jump_position) {
                    const uint64_t interp_len = end_block->interp_length_samples.value_or(plan.object_smoothing_frames);
                    if (interp_len > 0U) {
                        copy_windowed_object_input(
                            src, in_block.data(), num_in_ch, chunk_start, frames_now, ch_in.data());
                        const float* conv_in = ch_in.data();
                        if (src.diffuse_bus) {
                            decorrelate_diffuse_mono(diffuse_delay[si], ch_in.data(), frames_now, diffuse_in.data());
                            conv_in = diffuse_in.data();
                        }
                        const auto start_hrtf = hrtf_for_dir(*bs, start_block->az, start_block->el);
                        const auto end_hrtf = hrtf_for_dir(*bs, end_block->az, end_block->el);
                        convolve_crossfaded_object_block(hfft,
                                                         *bs,
                                                         conv_in,
                                                         frames_now,
                                                         src.gain * start_block->block_gain,
                                                         src.gain * end_block->block_gain,
                                                         start_hrtf,
                                                         end_hrtf,
                                                         ola[si],
                                                         l_buf.data(),
                                                         r_buf.data());
                        continue;
                    }
                }
            }

            uint64_t cursor = chunk_start;
            std::size_t bi = first_relevant_block(src, chunk_start);

            while (cursor < chunk_end) {
                while (bi < src.blocks.size() && src.blocks[bi].end_sample <= cursor) {
                    ++bi;
                }

                if (bi >= src.blocks.size() || src.blocks[bi].start_sample >= chunk_end) {
                    const auto off = static_cast<std::size_t>(cursor - chunk_start);
                    advance_silence(ola[si], chunk_end - cursor, l_buf.data() + off, r_buf.data() + off);
                    break;
                }

                const auto& blk = src.blocks[bi];
                if (cursor < blk.start_sample) {
                    const uint64_t silent_end = std::min<uint64_t>(blk.start_sample, chunk_end);
                    const auto off = static_cast<std::size_t>(cursor - chunk_start);
                    advance_silence(ola[si], silent_end - cursor, l_buf.data() + off, r_buf.data() + off);
                    cursor = silent_end;
                    continue;
                }

                const uint64_t seg_end = std::min<uint64_t>(blk.end_sample, chunk_end);
                if (seg_end <= cursor) {
                    ++bi;
                    continue;
                }

                const auto off = static_cast<std::size_t>(cursor - chunk_start);
                const uint64_t seg_frames = seg_end - cursor;
                const auto seg_fn = static_cast<std::size_t>(seg_frames);

                const uint16_t ic = src.channel_index;
                for (std::size_t f = 0; f < seg_fn; ++f) {
                    ch_in[f] = (ic < num_in_ch) ? in_block[((off + f) * num_in_ch) + ic] : 0.0F;
                }

                const float gain = src.gain * blk.block_gain;
                if (src.bypass_lfe) {
                    for (std::size_t f = 0; f < seg_fn; ++f) {
                        const float sample = ch_in[f] * gain;
                        l_buf[off + f] += sample;
                        r_buf[off + f] += sample;
                    }
                } else {
                    const float* conv_in = ch_in.data();
                    if (src.diffuse_bus) {
                        decorrelate_diffuse_mono(diffuse_delay[si], ch_in.data(), seg_frames, diffuse_in.data());
                        conv_in = diffuse_in.data();
                    }
                    const auto hrtf = hrtf_for_dir(*bs, blk.az, blk.el);
                    convolve_and_accumulate(
                        hfft, *bs, conv_in, seg_frames, gain, hrtf, ola[si], l_buf.data() + off, r_buf.data() + off);
                }

                cursor = seg_end;
                if (cursor >= blk.end_sample) {
                    ++bi;
                }
            }
        }

        // Interleave L/R into output block.
        for (std::size_t f = 0; f < fn; ++f) {
            out_block[(f * 2U) + 0U] = l_buf[f];
            out_block[(f * 2U) + 1U] = r_buf[f];
        }

        if (lufs_st) {
            ebur128_add_frames_float(lufs_st.get(), out_block.data(), fn);
        }
        if (writer.write(out_block.data(), frames_now) != frames_now) {
            return make_error(ErrorCode::io_error, "short write during binaural render", "output=" + plan.output_path);
        }

        frames_done += frames_now;
        const double frac = 0.2 + (0.7 * (static_cast<double>(frames_done) / static_cast<double>(num_frames)));
        progress.on_progress({RenderStage::rendering, frac, "rendering"});
    }

    progress.on_progress({RenderStage::finished, 1.0, "done"});
    logs.log(LogLevel::info, "binaural", fmt::format("wrote {} frames to {}", num_frames, plan.output_path));

    RenderMetrics metrics;
    if (lufs_st) {
        double loudness = 0.0;
        if (ebur128_loudness_global(lufs_st.get(), &loudness) == EBUR128_SUCCESS && std::isfinite(loudness)) {
            metrics.measured_lufs = loudness;
        }
        for (unsigned int ch = 0; ch < 2U; ++ch) {
            double ch_peak = 0.0;
            if (ebur128_true_peak(lufs_st.get(), ch, &ch_peak) == EBUR128_SUCCESS) {
                metrics.measured_peak_dbtp =
                    std::max(metrics.measured_peak_dbtp.value_or(-200.0), 20.0 * std::log10(std::max(ch_peak, 1e-10)));
            }
        }
    }
    return metrics;
}

} // namespace

CapabilityReport binaural_capabilities() {
    CapabilityReport r;
    r.backend_name = "binaural-hrtf";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    r.supported_layouts = {
        // clang-format off
        {"0+2+0", "Binaural (KEMAR or user SOFA HRIR)", 2, false, 0, true, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

} // namespace mradm
