#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ebur128.h>
#include <exception>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

#include "binaural_internal.h"
#include "binaural_spreader.h"
#include "render_common.h"

namespace mradm {

// The module-private HRTF state and interpolation entry points live in binaural_internal so the
// test-only probe (binaural_test_probe.cpp) can share them without compiling test code into the
// production library. Bring them into scope for the renderer code below.
using namespace binaural_internal;

// This block reopens binaural_internal (declared in binaural_internal.h): the renderer's private
// helpers, HRTF state and interpolation routines all live here so the test probe can reuse the
// declared subset without test code entering the production library.
namespace binaural_internal {

// ── Constants ────────────────────────────────────────────────────────────────

constexpr uint64_t k_min_block_size = 1024U;
constexpr float k_spreader_extent_threshold_deg = 1.0F; // min extent to route via saf_spreader
constexpr int k_n_azi = 361;                            // (int)(360/1 + 0.5) + 1
constexpr int k_n_elev = 181;                           // (int)(180/1 + 0.5) + 1
constexpr std::size_t k_binaural_divergence_slots = 3U;
constexpr std::size_t k_binaural_center_slot = 1U;
constexpr std::size_t k_binaural_extent_slots = 17U;
constexpr std::size_t k_binaural_extent_center_slot = 0U;
constexpr std::size_t k_diffuse_delay_len = 32U;

class TrackWorkerPool {
  public:
    explicit TrackWorkerPool(std::size_t worker_count) {
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    TrackWorkerPool(const TrackWorkerPool&) = delete;
    TrackWorkerPool& operator=(const TrackWorkerPool&) = delete;
    TrackWorkerPool(TrackWorkerPool&&) = delete;
    TrackWorkerPool& operator=(TrackWorkerPool&&) = delete;

    ~TrackWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    template <typename Fn> void parallel_for(std::size_t count, Fn&& fn) {
        if (count == 0U) {
            return;
        }
        if (workers_.empty() || count == 1U) {
            for (std::size_t i = 0; i < count; ++i) {
                fn(i);
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::forward<Fn>(fn);
            next_index_ = 0U;
            end_index_ = count;
            active_workers_ = 0U;
            error_ = nullptr;
        }
        work_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [&] { return next_index_ >= end_index_ && active_workers_ == 0U; });
        task_ = nullptr;
        // cppcheck-suppress knownConditionTrueFalse; worker threads may set error_ while this thread waits.
        if (error_ != nullptr) {
            std::rethrow_exception(error_);
        }
    }

  private:
    void worker_loop() {
        while (true) {
            std::size_t index = 0U;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [&] { return stopping_ || next_index_ < end_index_; });
                if (stopping_) {
                    return;
                }
                index = next_index_++;
                ++active_workers_;
            }

            try {
                task_(index);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error_ == nullptr) {
                    error_ = std::current_exception();
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_workers_;
                if (next_index_ >= end_index_ && active_workers_ == 0U) {
                    done_cv_.notify_one();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::function<void(std::size_t)> task_;
    std::exception_ptr error_;
    std::size_t next_index_{0U};
    std::size_t end_index_{0U};
    std::size_t active_workers_{0U};
    bool stopping_{false};
};

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
// HrtfDataset and BinauralState are declared in binaural_internal.h (shared with the test probe).

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

struct SafFree {
    void operator()(void* ptr) const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
        std::free(ptr);
    }
};
using SafFloatPtr = std::unique_ptr<float, SafFree>;
using SafIntPtr = std::unique_ptr<int, SafFree>;

void compress_vbap_rows(
    const float* rows, int row_stride, int num_dirs, std::size_t row_count, float* out_gains, int* out_dirs) {
    for (std::size_t row = 0; row < row_count; ++row) {
        std::array<float, 3> gains{};
        std::array<int, 3> dirs{};
        float gains_sum = 0.0F;
        int nonzero = 0;
        const float* row_data = rows + (row * static_cast<std::size_t>(row_stride));
        for (int dir = 0; dir < num_dirs; ++dir) {
            const float gain = row_data[dir];
            if (gain > 0.0000001F && nonzero < 3) {
                gains.at(static_cast<std::size_t>(nonzero)) = gain;
                dirs.at(static_cast<std::size_t>(nonzero)) = dir;
                gains_sum += gain;
                ++nonzero;
            }
        }
        for (int i = 0; i < nonzero; ++i) {
            out_gains[(row * 3U) + static_cast<std::size_t>(i)] =
                gains_sum > 0.0F ? std::max(gains.at(static_cast<std::size_t>(i)) / gains_sum, 0.0F) : 0.0F;
            out_dirs[(row * 3U) + static_cast<std::size_t>(i)] = dirs.at(static_cast<std::size_t>(i));
        }
    }
}

[[nodiscard]] bool build_compressed_vbap_grid(BinauralState& bs) {
    // SAF's generateVBAPgainTable3D() materialises the full grid x HRTF-dir table
    // before compression. For the built-in 1-degree KEMAR grid that transient is
    // 65,341 x 836 floats (about 208 MiB), which dominates max RSS. Build the same
    // table in modest batches, compress each batch immediately, and keep only the
    // 3-gain compressed representation used by compute_hrtf_into().
    constexpr float k_add_dummy_limit_deg = 60.0F; // mirrors SAF ADD_DUMMY_LIMIT
    constexpr std::size_t k_batch_grid_points = 2048U;

    std::vector<float> triangulation_dirs = bs.grid_dirs_deg;
    bool need_bottom_dummy = true;
    bool need_top_dummy = true;
    for (int i = 0; i < bs.num_dirs; ++i) {
        const float el = bs.grid_dirs_deg[(static_cast<std::size_t>(i) * 2U) + 1U];
        if (el <= -k_add_dummy_limit_deg) {
            need_bottom_dummy = false;
        }
        if (el >= k_add_dummy_limit_deg) {
            need_top_dummy = false;
        }
    }
    if (need_bottom_dummy) {
        triangulation_dirs.push_back(0.0F);
        triangulation_dirs.push_back(-90.0F);
    }
    if (need_top_dummy) {
        triangulation_dirs.push_back(0.0F);
        triangulation_dirs.push_back(90.0F);
    }

    float* out_vertices = nullptr;
    int* out_faces = nullptr;
    int num_out_vertices = 0;
    int num_out_faces = 0;
    findLsTriplets(triangulation_dirs.data(),
                   static_cast<int>(triangulation_dirs.size() / 2U),
                   /*omitLargeTriangles=*/1,
                   &out_vertices,
                   &num_out_vertices,
                   &out_faces,
                   &num_out_faces);
    SafFloatPtr out_vertices_guard{out_vertices};
    SafIntPtr out_faces_guard{out_faces};
    if (out_vertices == nullptr || out_faces == nullptr || num_out_faces <= 0) {
        return false;
    }

    float* layout_inv_mtx = nullptr;
    invertLsMtx3D(out_vertices, out_faces, num_out_faces, &layout_inv_mtx);
    SafFloatPtr layout_inv_guard{layout_inv_mtx};
    if (layout_inv_mtx == nullptr) {
        return false;
    }

    const std::size_t n_gtable = static_cast<std::size_t>(k_n_azi) * static_cast<std::size_t>(k_n_elev);
    bs.vbap_gains.assign(n_gtable * 3U, 0.0F);
    bs.vbap_dirs.assign(n_gtable * 3U, 0);

    std::vector<float> batch_dirs;
    batch_dirs.reserve(k_batch_grid_points * 2U);
    for (std::size_t base = 0; base < n_gtable; base += k_batch_grid_points) {
        const std::size_t count = std::min(k_batch_grid_points, n_gtable - base);
        batch_dirs.resize(count * 2U);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t grid = base + i;
            const auto az_idx = static_cast<int>(grid % static_cast<std::size_t>(k_n_azi));
            const auto el_idx = static_cast<int>(grid / static_cast<std::size_t>(k_n_azi));
            batch_dirs[(i * 2U) + 0U] = -180.0F + static_cast<float>(az_idx);
            batch_dirs[(i * 2U) + 1U] = -90.0F + static_cast<float>(el_idx);
        }

        float* batch_table = nullptr;
        vbap3D(batch_dirs.data(),
               static_cast<int>(count),
               num_out_vertices,
               out_faces,
               num_out_faces,
               /*spread=*/0.0F,
               layout_inv_mtx,
               &batch_table);
        SafFloatPtr batch_table_guard{batch_table};
        if (batch_table == nullptr) {
            return false;
        }
        compress_vbap_rows(batch_table,
                           num_out_vertices,
                           bs.num_dirs,
                           count,
                           bs.vbap_gains.data() + (base * 3U),
                           bs.vbap_dirs.data() + (base * 3U));
    }
    return true;
}

// Build BinauralState once.  Returns nullptr on VBAP triangulation failure.
std::unique_ptr<BinauralState> build_binaural_state(HrtfDataset dataset, uint64_t block_size) {
    auto bs = std::make_unique<BinauralState>();
    bs->num_dirs = dataset.num_dirs;
    bs->hrir_len = dataset.hrir_len;
    bs->fft_size = next_pow2_int(static_cast<int>(block_size) + dataset.hrir_len - 1);
    bs->n_bands = (bs->fft_size / 2) + 1;
    bs->overlap_len = dataset.hrir_len - 1;
    bs->dataset_name = std::move(dataset.name);
    bs->hrtf_td = std::move(dataset.hrirs);
    bs->grid_dirs_deg = std::move(dataset.dirs_deg);
    bs->sample_rate = dataset.sample_rate;

    // Convert HRIRs to frequency-domain HRTFs.
    bs->hrtf_fd.resize(static_cast<std::size_t>(bs->n_bands) * k_n_ears * static_cast<std::size_t>(bs->num_dirs));
    HRIRs2HRTFs(bs->hrtf_td.data(), bs->num_dirs, bs->hrir_len, bs->fft_size, bs->hrtf_fd.data());

    // Build compressed VBAP gain table for the HRTF measurement directions as "loudspeakers".
    if (!build_compressed_vbap_grid(*bs)) {
        return nullptr; // triangulation failed
    }

    return bs;
}

// Interpolate HRTF at (az_deg, el_deg) into an existing buffer (no allocation after first call).
//
// Magnitude-preserving, phase-from-complex interpolation. Direct complex VBAP weighting combs the
// far-ear high frequencies at off-grid directions (neighbouring HRTFs have different ITDs, so their
// complex spectra cancel where the phases misalign). To suppress that comb we take the MAGNITUDE
// from a weighted sum of |H| (real, non-negative → cannot comb) and the PHASE from the weighted sum
// of the complex spectra: out = (Σ wₖ|Hₖ|) · (Σ wₖHₖ)/|Σ wₖHₖ|.
//
// At a grid point exactly one VBAP gain is 1, so out == the raw measured HRTF (verified by the
// fixture test). Caveats — this is a comb-suppression heuristic, NOT a physically rigorous ITD /
// group-delay model: (1) |H| summation replaces every phase-driven cancellation with the average
// magnitude, so it also fills in genuine inter-direction notches, not just interpolation comb; it
// must not be tuned past the point where lateral grid points or real-content regressions appear.
// (2) The phase is unstable where |Σ wₖHₖ| → 0 and falls back to 0; it does not reconstruct true
// interpolated group delay. Constrained by the binaural fixture invariants (grid-point identity +
// off-grid far-ear improvement without lateral regression).
void compute_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<float_complex>& out) {
    const auto g = static_cast<std::size_t>(vbap_grid_idx(az_deg, el_deg));
    const auto gbase = g * 3U;
    const auto nd = static_cast<std::size_t>(bs.num_dirs);
    out.resize(static_cast<std::size_t>(bs.n_bands) * k_n_ears);
    for (int b = 0; b < bs.n_bands; ++b) {
        for (std::size_t ear = 0; ear < k_n_ears; ++ear) {
            float mag = 0.0F;
            float_complex cpx{0.0F, 0.0F};
            for (std::size_t k = 0; k < 3U; ++k) {
                const float gain = bs.vbap_gains[gbase + k];
                const auto dir = static_cast<std::size_t>(bs.vbap_dirs[gbase + k]);
                const auto h = bs.hrtf_fd[(static_cast<std::size_t>(b) * k_n_ears * nd) + (ear * nd) + dir];
                mag += gain * std::abs(h);
                cpx += gain * h;
            }
            const float acpx = std::abs(cpx);
            out[(static_cast<std::size_t>(b) * k_n_ears) + ear] =
                acpx > 1e-9F ? cpx * (mag / acpx) : float_complex{mag, 0.0F};
        }
    }
}

std::vector<float_complex> hrtf_for_dir(const BinauralState& bs, float az_deg, float el_deg) {
    std::vector<float_complex> out;
    compute_hrtf_into(bs, az_deg, el_deg, out);
    return out;
}

struct HrtfCache {
    float cached_az{std::numeric_limits<float>::quiet_NaN()};
    float cached_el{std::numeric_limits<float>::quiet_NaN()};
    std::vector<float_complex> hrtf;
};

const std::vector<float_complex>& get_cached_hrtf(const BinauralState& bs, float az, float el, HrtfCache& cache) {
    if (cache.hrtf.empty() || az != cache.cached_az || el != cache.cached_el) {
        compute_hrtf_into(bs, az, el, cache.hrtf);
        cache.cached_az = az;
        cache.cached_el = el;
    }
    return cache.hrtf;
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

// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ConvolutionScratch {
    std::vector<float> buf;
    std::vector<float_complex> src_fd;
    std::vector<float_complex> out_fd;
    std::vector<float> y;
    std::vector<float> l_cf0; // crossfade start arm
    std::vector<float> r_cf0;
    std::vector<float> l_cf1; // crossfade end arm
    std::vector<float> r_cf1;

    void resize(const BinauralState& bs, std::size_t max_block) {
        buf.resize(static_cast<std::size_t>(bs.fft_size));
        src_fd.resize(static_cast<std::size_t>(bs.n_bands));
        out_fd.resize(static_cast<std::size_t>(bs.n_bands));
        y.resize(static_cast<std::size_t>(bs.fft_size));
        l_cf0.resize(max_block);
        r_cf0.resize(max_block);
        l_cf1.resize(max_block);
        r_cf1.resize(max_block);
    }
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// Per-source resources for parallel OLA processing (each source owns its own FFT handle,
// scratch buffers, and output accumulators so workers never alias each other).
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct PerSourceConvState {
    void* hfft{nullptr};
    ConvolutionScratch scratch;
    std::vector<float> l_out;
    std::vector<float> r_out;
    std::vector<float> ch_in;
    std::vector<float> diffuse_in;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

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
                             float* r_out,
                             ConvolutionScratch& scratch) {
    const auto fn = static_cast<std::size_t>(frames_now);

    // Zero-pad source block into FFT buffer and transform.
    std::ranges::fill(scratch.buf, 0.0F);
    std::copy_n(src, fn, scratch.buf.begin());
    saf_rfft_forward(hfft, scratch.buf.data(), scratch.src_fd.data());

    for (int ear = 0; ear < k_n_ears; ++ear) {
        // Frequency-domain multiply: src_fd × hrtf[:][ear].
        for (int band = 0; band < bs.n_bands; ++band) {
            scratch.out_fd[static_cast<std::size_t>(band)] =
                gain * scratch.src_fd[static_cast<std::size_t>(band)] *
                hrtf[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)];
        }
        saf_rfft_backward(hfft, scratch.out_fd.data(), scratch.y.data());

        auto& overlap_vec = (ear == 0) ? state.left() : state.right();
        float* overlap = overlap_vec.data();
        float* dst = (ear == 0) ? l_out : r_out;
        const auto overlap_len = static_cast<std::size_t>(bs.overlap_len);

        // Overlap-add: accumulated output = y[0..fn-1] + saved overlap.
        for (std::size_t f = 0; f < fn; ++f) {
            dst[f] += scratch.y[f] + (f < overlap_len ? overlap[f] : 0.0F);
        }
        // Save new overlap: y[fn..fn+overlap_len-1] plus any unemitted residual from the
        // old overlap.  When fn < overlap_len, overlap[fn..overlap_len-1] was never written
        // to dst; those samples belong at output positions [fn..overlap_len-1] relative to
        // this segment and must be carried forward.  Processing i in ascending order is safe:
        // the read index (fn+i) exceeds the write index (i) for all i < fn, so no aliasing.
        for (std::size_t i = 0; i < overlap_len; ++i) {
            const float residual = (fn + i < overlap_len) ? overlap[fn + i] : 0.0F;
            overlap[i] = scratch.y[fn + i] + residual;
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

// Maps ADM block extent (width/height/depth, each 0..1) to a saf_spreader cone
// half-angle in degrees. This is an *intentionally separate* mapping from VBAP's
// mdap_spread_degrees() and the binaural cloud spread_scale: it feeds the SAF
// spreader's OM spreading-cone parameter, which has different perceptual scaling
// than MDAP point clouds. Width is weighted more than height (the horizontal plane
// dominates spatial impression); depth adds an isotropic term. Note: this does NOT
// apply distance scaling — extent is taken directly from the block. Used both to
// gate point-source bypass (k_spreader_extent_threshold_deg) and to drive the cone.
float extent_spread_deg(const SceneObjectBlock& block) noexcept {
    const float depth_r = std::max(0.0F, block.depth) * 20.0F;
    const float width_r = (std::max(0.0F, block.width) * 60.0F) + depth_r;
    const float height_r = (std::max(0.0F, block.height) * 45.0F) + depth_r;
    return std::max(width_r, height_r) * 2.0F;
}

// NOLINTBEGIN(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)
struct SpreaderSubSource {
    float az{0.0F};
    float el{0.0F};
    float spread_deg{0.0F};
    float gain{1.0F};
};

struct SpreaderBlock {
    std::vector<SpreaderSubSource> sources;
    uint64_t start_sample{0};
    uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
};

struct SpreaderTrack {
    uint16_t channel_index{0};
    float object_gain{1.0F};
    std::vector<SpreaderBlock> blocks;
    int n_sources{1};
};

struct SpreaderLane {
    std::size_t track_slot{0};
    std::size_t source_index{0};
};

struct SpreaderGroup {
    std::vector<std::size_t> track_indices;
    std::vector<SpreaderLane> lanes;
};
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)

std::vector<SpreaderGroup> build_spreader_groups(const std::vector<SpreaderTrack>& tracks,
                                                 std::size_t target_group_count) {
    const auto adapter_max_lanes = static_cast<std::size_t>(BinauralSpreaderAdapter::max_sources());
    if (tracks.empty()) {
        return {};
    }
    target_group_count = std::max<std::size_t>(1U, target_group_count);
    const std::size_t total_lanes =
        std::accumulate(tracks.begin(), tracks.end(), std::size_t{0}, [&](std::size_t sum, const SpreaderTrack& track) {
            return sum + std::min(static_cast<std::size_t>(track.n_sources), adapter_max_lanes);
        });
    const bool memory_pressure = total_lanes > (target_group_count * 2U);
    const std::size_t target_lanes =
        memory_pressure
            ? std::max<std::size_t>(
                  1U, std::min(adapter_max_lanes, (total_lanes + target_group_count - 1U) / target_group_count))
            : 1U;
    std::vector<SpreaderGroup> groups;
    SpreaderGroup current;

    auto flush_current = [&] {
        if (!current.lanes.empty()) {
            groups.push_back(std::move(current));
            current = {};
        }
    };

    for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
        const std::size_t lane_count = std::min(static_cast<std::size_t>(tracks[ti].n_sources), adapter_max_lanes);
        if (lane_count == 0U) {
            continue;
        }
        // Prefer one adapter per track. Pack tracks only once lane count is well
        // beyond the parallel budget, where adapter memory starts to dominate.
        if (!current.lanes.empty() && current.lanes.size() + lane_count > target_lanes) {
            flush_current();
        }

        const std::size_t track_slot = current.track_indices.size();
        current.track_indices.push_back(ti);
        for (std::size_t si = 0; si < lane_count; ++si) {
            current.lanes.push_back({track_slot, si});
        }
        if (current.lanes.size() >= target_lanes) {
            flush_current();
        }
    }
    flush_current();
    return groups;
}

const SpreaderBlock*
active_spreader_block_at(const SpreaderTrack& track, uint64_t cursor, uint64_t chunk_end, uint64_t& next_boundary) {
    for (const auto& block : track.blocks) {
        if (block.end_sample <= cursor) {
            continue;
        }
        if (block.start_sample <= cursor) {
            next_boundary = std::min({next_boundary, block.end_sample, chunk_end});
            return &block;
        }
        next_boundary = std::min({next_boundary, block.start_sample, chunk_end});
        return nullptr;
    }
    return nullptr;
}

std::vector<SpreaderTrack> build_spreader_tracks(const AdmScene& scene, LogSink& logs) {
    std::vector<SpreaderTrack> tracks;
    const auto lock_speakers = binaural_lock_speakers();
    bool screen_ref_warned{false};
    for (const auto& obj : scene.objects) {
        if (obj.mute) {
            continue;
        }
        for (const auto& track : obj.tracks) {
            if (!track.channel_index.has_value() || track.blocks.empty()) {
                continue;
            }
            const auto channel_index = *track.channel_index;
            SpreaderTrack st;
            st.channel_index = channel_index;
            st.object_gain = obj.gain;
            for (const auto& blk : track.blocks) {
                const auto prepared =
                    render_common::prepare_object_block(blk, obj, lock_speakers, logs, "binaural", screen_ref_warned);
                SpreaderBlock sb;
                sb.start_sample = prepared.start_sample;
                sb.end_sample = prepared.end_sample;
                for (const auto& src : prepared.sources) {
                    const float diffuse = std::clamp(src.diffuse, 0.0F, 1.0F);
                    const float direct_scale = std::sqrt(1.0F - diffuse);
                    const float spread = extent_spread_deg(src);
                    if (direct_scale > 1.0e-4F && spread >= k_spreader_extent_threshold_deg) {
                        auto [az, el] = block_position(src);
                        sb.sources.push_back({az, el, spread, src.gain * direct_scale});
                    }
                }
                if (!sb.sources.empty()) {
                    st.n_sources = std::max(st.n_sources, static_cast<int>(sb.sources.size()));
                    st.blocks.push_back(std::move(sb));
                }
            }
            if (!st.blocks.empty()) {
                std::ranges::sort(st.blocks, {}, &SpreaderBlock::start_sample);
                tracks.push_back(std::move(st));
            }
        }
    }
    return tracks;
}

[[nodiscard]] std::vector<ExtentSource>
expand_binaural_extent(const SceneObjectBlock& block, float source_gain, BinauralSpreadMode spread_mode) {
    if (spread_mode == BinauralSpreadMode::none || spread_mode == BinauralSpreadMode::saf_spreader) {
        auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }
    const float distance = distance_from_position(block.position);
    const auto [width_radius, height_radius] =
        render_common::extent_disk_radii(block.width, block.height, block.depth, distance);
    if (width_radius <= 1.0e-4F && height_radius <= 1.0e-4F) {
        auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }

    // Shared 17-point disk cloud (render_common); the per-backend geometry below
    // (direction_from_position / normalize / polar_from_direction in double precision)
    // stays local and unchanged so the binaural output remains bit-identical.
    constexpr float k_deg2rad = static_cast<float>(std::numbers::pi) / 180.0F;
    const auto& k_samples = render_common::k_extent_disk_samples;

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
                                 std::vector<BinauralSource>& srcs,
                                 BinauralSpreadMode spread_mode) {
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
            const bool has_extent = extent_spread_deg(source_block) >= k_spreader_extent_threshold_deg;
            if (direct_scale > 1.0e-4F && (spread_mode != BinauralSpreadMode::saf_spreader || !has_extent)) {
                const BinauralSpreadMode direct_mode =
                    (spread_mode == BinauralSpreadMode::saf_spreader) ? BinauralSpreadMode::none : spread_mode;
                for (const auto& extent_source :
                     expand_binaural_extent(source_block, source_block.gain * direct_scale, direct_mode)) {
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
            const BinauralSpreadMode diff_mode =
                (spread_mode == BinauralSpreadMode::saf_spreader) ? BinauralSpreadMode::none : spread_mode;
            for (const auto& extent_source :
                 expand_binaural_extent(source_block, source_block.gain * diffuse_scale, diff_mode)) {
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
std::vector<BinauralSource> build_sources(const AdmScene& scene, LogSink& logs, BinauralSpreadMode spread_mode) {
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
                append_object_track_sources(
                    obj, track, channel_index, lock_speakers, screen_ref_warned, logs, srcs, spread_mode);
            }

            // DirectSpeakers-type blocks.
            for (const auto& ds : track.ds_blocks) {
                const bool is_lfe = render_common::direct_speakers_block_is_lfe(ds);
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
                                      float* r_out,
                                      ConvolutionScratch& scratch) {
    OLAState start_state = state;
    OLAState end_state = state;
    const auto fn = static_cast<std::size_t>(frames_now);
    std::fill_n(scratch.l_cf0.begin(), fn, 0.0F);
    std::fill_n(scratch.r_cf0.begin(), fn, 0.0F);
    std::fill_n(scratch.l_cf1.begin(), fn, 0.0F);
    std::fill_n(scratch.r_cf1.begin(), fn, 0.0F);

    convolve_and_accumulate(hfft,
                            bs,
                            src,
                            frames_now,
                            start_gain,
                            start_hrtf,
                            start_state,
                            scratch.l_cf0.data(),
                            scratch.r_cf0.data(),
                            scratch);
    convolve_and_accumulate(
        hfft, bs, src, frames_now, end_gain, end_hrtf, end_state, scratch.l_cf1.data(), scratch.r_cf1.data(), scratch);

    for (std::size_t f = 0; f < fn; ++f) {
        const float alpha = fn > 1U ? static_cast<float>(f) / static_cast<float>(fn - 1U) : 0.0F;
        l_out[f] += (scratch.l_cf0[f] * (1.0F - alpha)) + (scratch.l_cf1[f] * alpha);
        r_out[f] += (scratch.r_cf0[f] * (1.0F - alpha)) + (scratch.r_cf1[f] * alpha);
    }
    state = std::move(end_state);
}

// ── Renderer ──────────────────────────────────────────────────────────────────

// Immutable, reusable binaural state: the HRTF tables + VBAP grid (the expensive
// SOFA load + FFT) and the scene-derived source / spreader-group lists. Reused across
// render_window() calls (PreviewSession scrubbing). The per-output stateful pieces
// (primed spreader adapters, OLA / diffuse-delay state, scratch) are rebuilt per call.
struct BinauralPrepared final : IPreparedRender {
    std::unique_ptr<BinauralState> bs;
    std::vector<BinauralSource> sources;
    std::vector<SpreaderTrack> spreader_tracks;
    std::vector<SpreaderGroup> spreader_groups;
    uint64_t render_block_size{0};
};

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;
};

CapabilityReport BinauralRenderer::capabilities() const {
    return binaural_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> BinauralRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    const auto& info = plan.scene.info;

    if (info.sample_rate != 48000U) {
        return make_error(ErrorCode::invalid_argument,
                          fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                          "input=" + plan.input_path);
    }

    const uint64_t render_block_size = std::max<uint64_t>(k_min_block_size, plan.object_smoothing_frames);

    // One-time setup: load HRIR dataset, then build HRTF table + VBAP grid.
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

    auto sources = build_sources(plan.scene, logs, plan.binaural_spread_mode);
    if (sources.empty()) {
        logs.log(LogLevel::warning, "binaural", "no renderable tracks found, writing silence");
    }
    logs.log(LogLevel::info, "binaural", fmt::format("{} source(s) to render", sources.size()));

    // Build spreader track + group config for saf_spreader mode (the stateful, primed
    // adapters are built per render in render_window since their STFT state is per-output).
    std::vector<SpreaderTrack> spreader_tracks;
    std::vector<SpreaderGroup> spreader_groups;
    if (plan.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
        const auto hw_threads = static_cast<std::size_t>(std::thread::hardware_concurrency());
        const std::size_t parallel_budget = (hw_threads > 0U) ? hw_threads : sources.size();
        spreader_tracks = build_spreader_tracks(plan.scene, logs);
        spreader_groups = build_spreader_groups(spreader_tracks, std::max<std::size_t>(1U, parallel_budget));
        logs.log(
            LogLevel::info,
            "binaural",
            fmt::format("{} spreader track(s) in {} adapter group(s) (saf_spreader, compensated latency {} samples)",
                        spreader_tracks.size(),
                        spreader_groups.size(),
                        BinauralSpreaderAdapter::total_latency()));
    }

    auto prepared = std::make_shared<BinauralPrepared>();
    prepared->bs = std::move(bs);
    prepared->sources = std::move(sources);
    prepared->spreader_tracks = std::move(spreader_tracks);
    prepared->spreader_groups = std::move(spreader_groups);
    prepared->render_block_size = render_block_size;
    return std::static_pointer_cast<IPreparedRender>(prepared);
}

// NOLINTNEXTLINE(readability-function-size)
Result<RenderMetrics> BinauralRenderer::render_window(const IPreparedRender& prep,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) {
    const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
    if (prepared == nullptr) {
        return make_error(
            ErrorCode::internal_error, "binaural: render_window received an incompatible prepared state", {});
    }
    const auto& info = plan.scene.info;
    const auto& bs = prepared->bs;
    const auto& sources = prepared->sources;
    const auto& spreader_tracks = prepared->spreader_tracks;
    const auto& spreader_groups = prepared->spreader_groups;
    const uint64_t render_block_size = prepared->render_block_size;
    const auto hw_threads = static_cast<std::size_t>(std::thread::hardware_concurrency());

    // Per-output (stateful, primed) spreader adapters, rebuilt from the prepared HRTF
    // tables + group config each render since their STFT state is per-output.
    std::vector<BinauralSpreaderAdapter> spreader_adapters;
    if (plan.binaural_spread_mode == BinauralSpreadMode::saf_spreader) {
        spreader_adapters.reserve(spreader_groups.size());
        std::ranges::transform(spreader_groups, std::back_inserter(spreader_adapters), [&](const SpreaderGroup& group) {
            return BinauralSpreaderAdapter{bs->hrtf_td.data(),
                                           bs->grid_dirs_deg.data(),
                                           bs->num_dirs,
                                           bs->hrir_len,
                                           bs->sample_rate,
                                           static_cast<int>(group.lanes.size())};
        });
        // Prime each adapter: pre-fill the output ring with one frame so process_chunk
        // drains exactly n_frames (constant total_latency(), no gaps).
        for (auto& adapter : spreader_adapters) {
            adapter.prime();
        }
    }

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

    // Per-source OLA state.
    std::vector<OLAState> ola;
    ola.reserve(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        ola.emplace_back(bs->overlap_len);
    }
    std::vector<DiffuseDelayState> diffuse_delay(sources.size());
    std::vector<HrtfCache> hrtf_cache(sources.size());

    // Per-source FFT handles, scratch, and output buffers for parallel OLA processing.
    std::vector<PerSourceConvState> src_cs(sources.size());
    for (auto& cs : src_cs) {
        saf_rfft_create(&cs.hfft, bs->fft_size);
        cs.scratch.resize(*bs, static_cast<std::size_t>(render_block_size));
        cs.l_out.resize(static_cast<std::size_t>(render_block_size));
        cs.r_out.resize(static_cast<std::size_t>(render_block_size));
        cs.ch_in.resize(static_cast<std::size_t>(render_block_size));
        cs.diffuse_in.resize(static_cast<std::size_t>(render_block_size));
    }
    struct SrcCsGuard {
        explicit SrcCsGuard(std::vector<PerSourceConvState>& d) : data(d) {}
        SrcCsGuard(const SrcCsGuard&) = delete;
        SrcCsGuard& operator=(const SrcCsGuard&) = delete;
        SrcCsGuard(SrcCsGuard&&) = delete;
        SrcCsGuard& operator=(SrcCsGuard&&) = delete;
        ~SrcCsGuard() {
            for (auto& cs : data) {
                if (cs.hfft != nullptr) {
                    saf_rfft_destroy(&cs.hfft);
                }
            }
        }

      private:
        std::vector<PerSourceConvState>& data;
    } src_cs_guard{src_cs};

    const uint64_t num_frames = info.num_frames;
    const uint16_t num_in_ch = info.num_channels;

    std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * render_block_size);
    std::vector<float> out_block(2U * render_block_size); // interleaved L/R
    uint64_t frames_done = 0;

    // saf_spreader latency compensation: after prime() the spreader has a constant
    // total_latency() sample delay (= STFT processing_delay() + a one-frame ring
    // cushion; prime() guarantees every process_chunk() drains exactly n_frames).
    // To keep its output sample-aligned with the OLA (point/diffuse) path and with
    // the input ADM timeline, we delay the OLA contribution by spr_delay (ola_dl ring),
    // skip the first spr_delay output samples (out_skip), and run a spr_delay-sample
    // silent tail after the input is exhausted. The cushion makes spr_delay >= STFT
    // delay + max partial-batch carry, so the tail also fully flushes non-512-aligned
    // input lengths. Net result: file length == num_frames, fully aligned.
    const bool spreader_mode = !spreader_adapters.empty();
    const std::size_t spr_delay =
        spreader_mode ? static_cast<std::size_t>(BinauralSpreaderAdapter::total_latency()) : 0U;
    const std::size_t spreader_worker_count =
        (spreader_groups.size() > 1U && hw_threads > 1U) ? std::min(spreader_groups.size(), hw_threads) : 0U;
    TrackWorkerPool spreader_pool(spreader_worker_count);
    const std::size_t ola_worker_count =
        (sources.size() > 1U && hw_threads > 1U) ? std::min(sources.size(), hw_threads) : 0U;
    TrackWorkerPool ola_pool(ola_worker_count);
    std::vector<float> ola_dl_l;
    std::vector<float> ola_dl_r;
    std::size_t ola_dl_pos = 0;
    if (spreader_mode) {
        ola_dl_l.assign(spr_delay, 0.0F);
        ola_dl_r.assign(spr_delay, 0.0F);
    }
    std::size_t out_skip = spr_delay;
    uint64_t out_written = 0;

    // On-demand output window (RenderPlan::render_window). emit() writes only output
    // frames inside [win_start, win_end) on the absolute output timeline; out_abs
    // tracks that position (the next produced sample after the out_skip latency
    // discard). The default (cloud/none) path additionally seeks the input and
    // pre-rolls enough aligned blocks to converge the OLA overlap + diffuse delay,
    // so the window is bit-identical to a full render then sliced. The experimental
    // saf_spreader path keeps start_pos=0 (full STFT warm-up) and only trims the
    // output, since reconstructing the spreader STFT state mid-stream is not worth the
    // risk. When not windowed, win_start=0 / win_end=num_frames reproduces the full
    // render exactly.
    const bool windowed = plan.render_window.has_value();
    const uint64_t win_start = windowed ? std::min(plan.render_window->start_frame, num_frames) : 0;
    const uint64_t win_end = windowed ? std::min(win_start + plan.render_window->frame_count, num_frames) : num_frames;
    uint64_t start_pos = 0;
    if (windowed && !spreader_mode && win_start > 0) {
        const auto warmup_frames =
            std::max<uint64_t>({render_block_size, static_cast<uint64_t>(bs->overlap_len), k_diffuse_delay_len});
        const uint64_t warmup_blocks = (warmup_frames + render_block_size - 1U) / render_block_size;
        const uint64_t start_block = win_start / render_block_size;
        start_pos = (start_block > warmup_blocks) ? ((start_block - warmup_blocks) * render_block_size) : 0;
    }
    if (start_pos > 0) {
        render_common::seek_reader_abs(*reader, start_pos);
    }
    frames_done = start_pos;      // input cursor
    uint64_t out_abs = start_pos; // absolute output-timeline position of the next produced sample

    // Skip out_skip leading (latency) samples, then write only the produced frames that
    // fall inside [win_start, win_end), interleaving L/R and metering them. Used by both
    // the main loop and the spreader tail drain. Returns false on short write.
    auto emit = [&](const float* lb, const float* rb, std::size_t count) -> bool {
        std::size_t local = 0;
        if (out_skip > 0) {
            const std::size_t s = std::min(out_skip, count);
            out_skip -= s;
            local = s;
        }
        const std::size_t avail = count - local;
        if (avail == 0) {
            return true;
        }
        const uint64_t base = out_abs; // absolute output frame of the first available sample
        out_abs += avail;
        const uint64_t lo = std::max(base, win_start);
        const uint64_t hi = std::min(base + avail, win_end);
        if (hi <= lo) {
            return true; // this chunk lies entirely outside the window
        }
        const std::size_t src_off = local + static_cast<std::size_t>(lo - base);
        const auto want = static_cast<std::size_t>(hi - lo);
        for (std::size_t f = 0; f < want; ++f) {
            out_block[(f * 2U) + 0U] = lb[src_off + f];
            out_block[(f * 2U) + 1U] = rb[src_off + f];
        }
        if (lufs_st) {
            ebur128_add_frames_float(lufs_st.get(), out_block.data(), want);
        }
        if (writer.write(out_block.data(), want) != want) {
            return false;
        }
        out_written += want;
        return true;
    };

    progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.2, 0.0, 0, 0, "rendering"});

    while (frames_done < num_frames && out_abs < win_end) {
        if (plan.cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
        }
        const uint64_t frames_now = std::min(render_block_size, num_frames - frames_done);
        const auto fn = static_cast<std::size_t>(frames_now);

        reader->read(in_block.data(), frames_now);
        std::ranges::fill(out_block, 0.0F);

        // Scratch L/R accumulation buffers (non-interleaved for SIMD friendliness).
        std::vector<float> l_buf(fn, 0.0F);
        std::vector<float> r_buf(fn, 0.0F);

        // In saf_spreader mode the OLA (point/diffuse) path accumulates into a
        // separate buffer so it can be delayed by spr_delay before being mixed
        // with the spreader output. Otherwise it writes directly to l_buf/r_buf.
        std::vector<float> ola_l;
        std::vector<float> ola_r;
        float* ola_l_dst = l_buf.data();
        float* ola_r_dst = r_buf.data();
        if (spreader_mode) {
            ola_l.assign(fn, 0.0F);
            ola_r.assign(fn, 0.0F);
            ola_l_dst = ola_l.data();
            ola_r_dst = ola_r.data();
        }

        // Process each OLA source independently; parallelise across sources.
        ola_pool.parallel_for(sources.size(), [&](std::size_t si) {
            const auto& src = sources[si];
            if (src.blocks.empty()) {
                return;
            }

            auto& cs = src_cs[si];
            float* src_l = cs.l_out.data();
            float* src_r = cs.r_out.data();
            std::fill_n(src_l, fn, 0.0F);
            std::fill_n(src_r, fn, 0.0F);

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
                            src, in_block.data(), num_in_ch, chunk_start, frames_now, cs.ch_in.data());
                        const float* conv_in = cs.ch_in.data();
                        if (src.diffuse_bus) {
                            decorrelate_diffuse_mono(
                                diffuse_delay[si], cs.ch_in.data(), frames_now, cs.diffuse_in.data());
                            conv_in = cs.diffuse_in.data();
                        }
                        const auto& start_hrtf = get_cached_hrtf(*bs, start_block->az, start_block->el, hrtf_cache[si]);
                        const auto end_hrtf = hrtf_for_dir(*bs, end_block->az, end_block->el);
                        convolve_crossfaded_object_block(cs.hfft,
                                                         *bs,
                                                         conv_in,
                                                         frames_now,
                                                         src.gain * start_block->block_gain,
                                                         src.gain * end_block->block_gain,
                                                         start_hrtf,
                                                         end_hrtf,
                                                         ola[si],
                                                         src_l,
                                                         src_r,
                                                         cs.scratch);
                        return;
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
                    advance_silence(ola[si], chunk_end - cursor, src_l + off, src_r + off);
                    break;
                }

                const auto& blk = src.blocks[bi];
                if (cursor < blk.start_sample) {
                    const uint64_t silent_end = std::min<uint64_t>(blk.start_sample, chunk_end);
                    const auto off = static_cast<std::size_t>(cursor - chunk_start);
                    advance_silence(ola[si], silent_end - cursor, src_l + off, src_r + off);
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
                    cs.ch_in[f] = (ic < num_in_ch) ? in_block[((off + f) * num_in_ch) + ic] : 0.0F;
                }

                const float gain = src.gain * blk.block_gain;
                if (src.bypass_lfe) {
                    for (std::size_t f = 0; f < seg_fn; ++f) {
                        const float sample = cs.ch_in[f] * gain;
                        src_l[off + f] += sample;
                        src_r[off + f] += sample;
                    }
                } else {
                    const float* conv_in = cs.ch_in.data();
                    if (src.diffuse_bus) {
                        decorrelate_diffuse_mono(diffuse_delay[si], cs.ch_in.data(), seg_frames, cs.diffuse_in.data());
                        conv_in = cs.diffuse_in.data();
                    }
                    const auto& hrtf = get_cached_hrtf(*bs, blk.az, blk.el, hrtf_cache[si]);
                    convolve_and_accumulate(
                        cs.hfft, *bs, conv_in, seg_frames, gain, hrtf, ola[si], src_l + off, src_r + off, cs.scratch);
                }

                cursor = seg_end;
                if (cursor >= blk.end_sample) {
                    ++bi;
                }
            }
        });

        // Reduce per-source outputs into the shared OLA destination.
        for (std::size_t si = 0; si < sources.size(); ++si) {
            const float* src_l = src_cs[si].l_out.data();
            const float* src_r = src_cs[si].r_out.data();
            for (std::size_t f = 0; f < fn; ++f) {
                ola_l_dst[f] += src_l[f];
                ola_r_dst[f] += src_r[f];
            }
        }

        // Delay the OLA (point/diffuse) contribution by spr_delay so it co-aligns
        // with the spreader's inherent STFT latency, then mix into l_buf/r_buf.
        if (spreader_mode) {
            for (std::size_t f = 0; f < fn; ++f) {
                l_buf[f] += ola_dl_l[ola_dl_pos];
                r_buf[f] += ola_dl_r[ola_dl_pos];
                ola_dl_l[ola_dl_pos] = ola_l[f];
                ola_dl_r[ola_dl_pos] = ola_r[f];
                ola_dl_pos = (ola_dl_pos + 1U) % spr_delay;
            }
        }

        // saf_spreader direct-source path: several independent ADM tracks may share one
        // SAF spreader adapter as source lanes, amortising the adapter's HRTF-derived
        // tables. Each adapter group accumulates into its own l/r scratch buffer;
        // results are reduced into l_buf/r_buf after all groups finish.
        if (!spreader_adapters.empty()) {
            const uint64_t chunk_start_sp = frames_done;
            const uint64_t chunk_end_sp = frames_done + frames_now;
            const std::size_t n_groups = spreader_groups.size();

            // Per-group output scratch (independent of l_buf/r_buf during parallel phase).
            std::vector<std::vector<float>> spr_l(n_groups, std::vector<float>(fn, 0.0F));
            std::vector<std::vector<float>> spr_r(n_groups, std::vector<float>(fn, 0.0F));

            auto process_one_group = [&](std::size_t gi) {
                const auto& group = spreader_groups[gi];
                auto& adapter = spreader_adapters[gi];
                const std::vector<float> zeros_full(fn, 0.0F);
                std::vector<float> track_ch(group.track_indices.size() * fn, 0.0F);
                std::vector<const float*> lane_ptrs(group.lanes.size(), zeros_full.data());
                std::vector<const SpreaderBlock*> active_blocks(group.track_indices.size(), nullptr);
                uint64_t sp_cursor = chunk_start_sp;
                while (sp_cursor < chunk_end_sp) {
                    uint64_t next_boundary = chunk_end_sp;
                    for (std::size_t ts = 0; ts < group.track_indices.size(); ++ts) {
                        active_blocks[ts] = active_spreader_block_at(
                            spreader_tracks[group.track_indices[ts]], sp_cursor, chunk_end_sp, next_boundary);
                    }
                    const auto seg_off = static_cast<std::size_t>(sp_cursor - chunk_start_sp);
                    const auto seg_fn = static_cast<std::size_t>(next_boundary - sp_cursor);
                    if (seg_fn == 0U) {
                        break;
                    }

                    for (std::size_t ts = 0; ts < group.track_indices.size(); ++ts) {
                        const auto* active = active_blocks[ts];
                        if (active == nullptr) {
                            continue;
                        }
                        const auto& st = spreader_tracks[group.track_indices[ts]];
                        const uint16_t ic = st.channel_index;
                        float* track_buf = track_ch.data() + (ts * fn);
                        for (std::size_t f = 0; f < seg_fn; ++f) {
                            track_buf[f] = (ic < num_in_ch) ? in_block[((seg_off + f) * num_in_ch) + ic] : 0.0F;
                        }
                    }

                    for (std::size_t li = 0; li < group.lanes.size(); ++li) {
                        const auto& lane = group.lanes[li];
                        const auto* active = active_blocks[lane.track_slot];
                        if (active != nullptr && lane.source_index < active->sources.size()) {
                            const auto& st = spreader_tracks[group.track_indices[lane.track_slot]];
                            const auto& ss = active->sources[lane.source_index];
                            adapter.set_source(
                                static_cast<int>(li), ss.az, ss.el, ss.spread_deg, st.object_gain * ss.gain);
                            lane_ptrs[li] = track_ch.data() + (lane.track_slot * fn);
                        } else {
                            lane_ptrs[li] = zeros_full.data();
                        }
                    }
                    adapter.process_chunk(lane_ptrs.data(),
                                          static_cast<int>(lane_ptrs.size()),
                                          seg_fn,
                                          spr_l[gi].data() + seg_off,
                                          spr_r[gi].data() + seg_off);
                    sp_cursor = next_boundary;
                }
            };

            spreader_pool.parallel_for(n_groups, process_one_group);

            // Reduce per-group scratch into l_buf / r_buf.
            for (std::size_t gi = 0U; gi < n_groups; ++gi) {
                for (std::size_t f = 0U; f < fn; ++f) {
                    l_buf[f] += spr_l[gi][f];
                    r_buf[f] += spr_r[gi][f];
                }
            }
        }

        // Head-skip (out_skip) + output-window clipping handled inside emit().
        if (!emit(l_buf.data(), r_buf.data(), fn)) {
            return make_error(ErrorCode::io_error, "short write during binaural render", "output=" + plan.output_path);
        }

        frames_done += frames_now;
        const uint64_t progress_done = std::min(frames_done, win_end) - start_pos;
        const uint64_t progress_total = std::max<uint64_t>(1, win_end - start_pos);
        const auto progress_span = static_cast<double>(progress_total);
        const double stage_fraction = static_cast<double>(progress_done) / progress_span;
        const double frac = 0.2 + (0.7 * stage_fraction);
        progress.on_progress({RenderStage::rendering,
                              RenderOperation::render_audio,
                              frac,
                              stage_fraction,
                              progress_done,
                              progress_total,
                              "rendering"});
    }

    // saf_spreader tail: the input is exhausted but spr_delay samples of real
    // output are still in flight (STFT latency + delayed OLA path). Feed spr_delay
    // silent samples through both paths and write whichever samples fall inside the
    // requested output window. The leading warm-up was already dropped via out_skip.
    if (spreader_mode) {
        uint64_t tail_remaining = spr_delay;
        while (tail_remaining > 0 && out_abs < win_end) {
            const auto tn = static_cast<std::size_t>(std::min<uint64_t>(render_block_size, tail_remaining));
            std::vector<float> l_buf(tn, 0.0F);
            std::vector<float> r_buf(tn, 0.0F);
            // Drain the OLA delay ring (push silence, pop the held real tail).
            for (std::size_t f = 0; f < tn; ++f) {
                l_buf[f] += ola_dl_l[ola_dl_pos];
                r_buf[f] += ola_dl_r[ola_dl_pos];
                ola_dl_l[ola_dl_pos] = 0.0F;
                ola_dl_r[ola_dl_pos] = 0.0F;
                ola_dl_pos = (ola_dl_pos + 1U) % spr_delay;
            }
            // Drain the spreader STFT tail (feed silence; exact-D latency means
            // exactly tn aligned output samples come out).
            const std::vector<float> zeros_tn(tn, 0.0F);
            for (std::size_t gi = 0; gi < spreader_adapters.size(); ++gi) {
                const int ns = static_cast<int>(spreader_groups[gi].lanes.size());
                std::vector<const float*> zptrs(static_cast<std::size_t>(ns), zeros_tn.data());
                spreader_adapters[gi].process_chunk(zptrs.data(), ns, tn, l_buf.data(), r_buf.data());
            }
            if (!emit(l_buf.data(), r_buf.data(), tn)) {
                return make_error(
                    ErrorCode::io_error, "short write during binaural spreader tail", "output=" + plan.output_path);
            }
            tail_remaining -= tn;
        }
    }

    progress.on_progress({RenderStage::finished, RenderOperation::finish, 1.0, 1.0, 0, 0, "done"});
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("wrote {} frames to {}{}",
                         out_written,
                         plan.output_path,
                         windowed ? fmt::format(" (window [{}, {}) of {} frames)", win_start, win_end, num_frames)
                                  : std::string{}));

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

} // namespace binaural_internal

CapabilityReport binaural_capabilities() {
    CapabilityReport r;
    r.backend_name = "saf-binaural-hrtf";
    r.backend_version = "1.0";
    r.supports_objects = true;
    r.supports_direct_speakers = true;
    r.supports_hoa = false;
    r.supports_channel_lock = true;
    r.supports_object_divergence = true;
    r.supports_diffuse = true;
    // Default (cloud/none) path windows via seek + aligned pre-roll; the experimental
    // saf_spreader path keeps full STFT warm-up and only trims output.
    r.supports_render_window = true;
    r.supported_layouts = {
        // clang-format off
        {"0+2+0", "SAF HRTF binaural (KEMAR or user SOFA HRIR)", 2, false, 0, true, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

bool binaural_sofa_supported() {
#ifdef SAF_ENABLE_SOFA_READER_MODULE
    return true;
#else
    return false;
#endif
}

} // namespace mradm
