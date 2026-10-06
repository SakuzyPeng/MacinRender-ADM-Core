#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

#include "binaural_dsp.h"
#include "binaural_internal.h"
#include "binaural_spreader.h"
#include "consistency_trace.h"
#include "dsp.h"
#include "head_rotation.h"
#include "meter.h"
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
constexpr std::size_t k_binaural_divergence_slots = 3U;
constexpr std::size_t k_binaural_center_slot = 1U;
constexpr std::size_t k_binaural_extent_slots = 17U;
constexpr std::size_t k_binaural_extent_center_slot = 0U;
constexpr std::size_t k_diffuse_delay_len = 32U;

constexpr std::size_t k_topology_diffuse = 0U;
constexpr std::size_t k_topology_width = 1U;
constexpr std::size_t k_topology_height = 2U;
constexpr std::size_t k_topology_depth = 3U;
constexpr std::size_t k_topology_divergence = 4U;
constexpr uint64_t k_live_topology_min_interval_ms = 100U;
using LiveTopologyScales = std::array<float, 5>;

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


// Grid index into the pre-computed VBAP table (az_res=1°, el_res=1°).
int vbap_grid_idx(float az_deg, float el_deg) {
    std::size_t index = 0U;
    std::array<char, 256> message{};
    dsp::check(mradm_dsp_hrtf_grid_index(az_deg, el_deg, &index, message.data(), message.size()), message.data());
    return static_cast<int>(index);
}


// ── Listener head rotation (head tracking / free-look) ──────────────────────────
// SAF binaural has no global field-rotation param like the Apple AUSpatialMixer; instead we
// rotate each (world-locked) source's direction into the head frame *before* the HRTF lookup.
// As the head turns, world-fixed sources move oppositely in head space → they stay put in the
// world. Head-locked sources skip this and keep their raw direction (glued to the head).
//
// Coordinate frame matches Vec3 (x=right, y=front, z=up) and the Apple backend's HeadVec, so the
// yaw/pitch/roll convention and composition order are identical to head_lock_compensate() — yaw
// (+left) about +z, pitch (+up) about +x, roll about +y, composed q_yaw·q_pitch·q_roll as
// head→world. We apply the inverse (world→head) so a source that is "front in the world" lands to
// the listener's right when they turn their head left.
using render_common::HeadRotation;

// Both built-in and user HRTFs enter through the Rust-owned dataset boundary.
Result<HrtfDataset> read_rust_dataset(std::span<const std::uint8_t> bytes, bool builtin) {
    void* raw = nullptr;
    std::array<char, 256> error{};
    auto status =
        mradm_dsp_dataset_create(bytes.data(), bytes.size(), builtin ? 1 : 0, &raw, error.data(), error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    const std::unique_ptr<void, decltype(&mradm_dsp_dataset_destroy)> owner(raw, mradm_dsp_dataset_destroy);
    MradmDspDatasetInfo info{};
    status = mradm_dsp_dataset_info(raw, &info, error.data(), error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    if (info.num_dirs > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        info.ir_len > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return make_error(ErrorCode::unsupported, "HRTF dataset exceeds renderer dimensions");
    }
    HrtfDataset result;
    result.sample_rate = static_cast<int>(info.sample_rate);
    result.num_dirs = static_cast<int>(info.num_dirs);
    result.hrir_len = static_cast<int>(info.ir_len);
    result.dirs_deg.resize(info.num_dirs * 2U);
    result.hrirs.resize(info.num_dirs * 2U * info.ir_len);
    std::array<char, 256> name{};
    status = mradm_dsp_dataset_copy(raw,
                                    result.dirs_deg.data(),
                                    result.dirs_deg.size(),
                                    result.hrirs.data(),
                                    result.hrirs.size(),
                                    name.data(),
                                    name.size(),
                                    error.data(),
                                    error.size());
    if (status != 0) {
        return make_error(static_cast<ErrorCode>(status), error.data());
    }
    result.name = name.data();
    return result;
}

HrtfDataset built_in_kemar_dataset() {
    auto result = read_rust_dataset({}, true);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(*result);
}

Result<HrtfDataset> load_sofa_dataset(const std::filesystem::path& path, std::uint32_t input_sample_rate) {
#if MR_ADM_ENABLE_SOFA
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return make_error(ErrorCode::io_error, "SOFA file could not be opened", "path=" + path.string());
    }
    const std::streamoff length = input.tellg();
    if (length <= 0) {
        return make_error(ErrorCode::io_error, "SOFA file is empty or unreadable");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length))) {
        return make_error(ErrorCode::io_error, "SOFA file could not be read completely");
    }
    auto result = read_rust_dataset(bytes, false);
    if (!result) {
        return result;
    }
    if (input_sample_rate != 0U && !std::cmp_equal(input_sample_rate, result->sample_rate)) {
        return make_error(
            ErrorCode::unsupported,
            fmt::format("SOFA: sample rate {} Hz does not match input {} Hz", result->sample_rate, input_sample_rate),
            "path=" + path.string());
    }
    result->name = fmt::format("SOFA {}", result->name.empty() ? path.filename().string() : result->name);
    return result;
#else
    (void) input_sample_rate;
    return make_error(ErrorCode::unsupported,
                      "SOFA loading is disabled in this build (MR_ADM_ENABLE_SOFA=OFF)",
                      "path=" + path.string());
#endif
}

Result<HrtfDataset> load_hrtf_dataset(const RenderPlan& plan) {
    if (plan.sofa_path.has_value()) {
        return load_sofa_dataset(*plan.sofa_path, plan.scene.info.sample_rate);
    }
    return built_in_kemar_dataset();
}

// Preparation publishes only fully validated, immutable Rust interpolation data.
Result<std::unique_ptr<BinauralState>>
build_binaural_state(HrtfDataset dataset, uint64_t block_size, bool cache_magnitudes) {
    constexpr std::size_t k_max_fft = std::size_t{1U} << 24U;
    if (dataset.num_dirs <= 0 || dataset.hrir_len <= 0 || block_size == 0U || block_size > k_max_fft ||
        static_cast<std::size_t>(dataset.hrir_len) > k_max_fft - block_size + 1U ||
        dataset.dirs_deg.size() != static_cast<std::size_t>(dataset.num_dirs) * 2U) {
        return make_error(ErrorCode::invalid_argument, "invalid HRTF preparation dimensions");
    }
    auto bs = std::make_unique<BinauralState>();
    bs->num_dirs = dataset.num_dirs;
    bs->hrir_len = dataset.hrir_len;
    bs->fft_size = static_cast<int>(
        std::bit_ceil(static_cast<std::size_t>(block_size) + static_cast<std::size_t>(dataset.hrir_len) - 1U));
    bs->n_bands = (bs->fft_size / 2) + 1;
    bs->overlap_len = dataset.hrir_len - 1;
    bs->dataset_name = std::move(dataset.name);
    bs->hrtf_td = std::move(dataset.hrirs);
    bs->grid_dirs_deg = std::move(dataset.dirs_deg);
    bs->sample_rate = dataset.sample_rate;
    bs->grid = prepare_hrtf_grid(bs->grid_dirs_deg);
    if (!bs->grid) {
        return make_error(ErrorCode::render_failed, "HRTF direction triangulation failed");
    }
    auto filters = dsp::HrtfFilters::create(*bs->grid,
                                            bs->hrtf_td,
                                            static_cast<std::size_t>(bs->hrir_len),
                                            static_cast<std::size_t>(bs->fft_size),
                                            cache_magnitudes);
    if (!filters) {
        return tl::unexpected{filters.error()};
    }
    bs->filters = std::move(*filters);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    const auto spectrum = bs->filters.spectrum_snapshot();
    const auto grid = bs->grid->snapshot();
    if (!spectrum) {
        return tl::unexpected{spectrum.error()};
    }
    if (!grid) {
        return tl::unexpected{grid.error()};
    }
    consistency::dump("binaural.01-hrir.f32", bs->hrtf_td);
    consistency::dump("binaural.02-grid.f32", bs->grid_dirs_deg);
    consistency::dump("binaural.03-hrtf.c32", *spectrum);
    consistency::dump("binaural.04-vbap-gains.f32", grid->gains);
    consistency::dump("binaural.05-vbap-dirs.i32", grid->directions);
#endif
    return bs;
}

// Private renderer adapters reuse caller buffers; interpolation and all
// frequency-domain state are owned by Rust. Diagnostics use that same kernel.
void compute_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out) {
    out.resize(bs.filters.output_size());
    const MradmDspHrtfTrace* trace = nullptr;
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    std::vector<float> trace_magnitudes(out.size() * 3U);
    std::vector<float> trace_scale(out.size() * 2U);
    std::vector<dsp::Complex> trace_sum(out.size());
    const MradmDspHrtfTrace buffers{trace_magnitudes.data(),
                                    trace_magnitudes.size(),
                                    reinterpret_cast<float*>(trace_sum.data()),
                                    trace_sum.size() * 2U,
                                    trace_scale.data(),
                                    trace_scale.size()};
    trace = &buffers;
#endif
    const auto result = bs.filters.query(az_deg, el_deg, dsp::HrtfLookup::quantized, out, trace);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    const auto g = vbap_grid_idx(az_deg, el_deg);
    consistency::dump("hrtf-grid-" + std::to_string(g) + ".c32", out);
    consistency::dump("hrtf-grid-" + std::to_string(g) + "-magnitudes.f32", trace_magnitudes);
    consistency::dump("hrtf-grid-" + std::to_string(g) + "-complex-sum.c32", trace_sum);
    consistency::dump("hrtf-grid-" + std::to_string(g) + "-scale.f32", trace_scale);
#endif
}

void compute_continuous_hrtf_into(const BinauralState& bs, float az_deg, float el_deg, std::vector<dsp::Complex>& out) {
    out.resize(bs.filters.output_size());
    const auto result = bs.filters.query(az_deg, el_deg, dsp::HrtfLookup::continuous, out);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
}

std::vector<dsp::Complex> hrtf_for_dir(const BinauralState& bs, float az_deg, float el_deg) {
    std::vector<dsp::Complex> out;
    compute_hrtf_into(bs, az_deg, el_deg, out);
    return out;
}

struct HrtfCache {
    float cached_az{std::numeric_limits<float>::quiet_NaN()};
    float cached_el{std::numeric_limits<float>::quiet_NaN()};
    std::vector<dsp::Complex> hrtf;
};

// Per-source memory of the last head-rotated direction, so a moving listener head crossfades the
// HRTF kernel block-to-block instead of switching it abruptly (which clicks). Invalidated whenever
// head tracking is inactive so re-activation re-seeds without morphing from a stale direction.
struct HeadSmoothState {
    float az{0.0F};
    float el{0.0F};
    bool valid{false};
};

const std::vector<dsp::Complex>& get_cached_hrtf(const BinauralState& bs, float az, float el, HrtfCache& cache) {
    if (cache.hrtf.empty() || az != cache.cached_az || el != cache.cached_el) {
        compute_hrtf_into(bs, az, el, cache.hrtf);
        cache.cached_az = az;
        cache.cached_el = el;
    }
    return cache.hrtf;
}

// Each worker owns one Rust OLA workspace and its C++ source/output buffers.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct PerSourceConvState {
    dsp::OlaConvolver convolver;
    std::vector<float> l_out;
    std::vector<float> r_out;
    std::vector<float> ch_in;
    std::vector<float> diffuse_in;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

// ── Source descriptor ─────────────────────────────────────────────────────────

// One renderable source extracted from the ADM scene.
struct BinauralSource {
    uint16_t channel_index{0};
    float gain{1.0F};              // object-level gain
    std::string object_id;         // owning SceneObject::id, for live gain overrides
    std::string speaker_label_key; // normalized DirectSpeakers label (empty for Objects); per-channel live gain key
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
        bool head_locked{false};
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
    bool head_locked{false};
};

struct SpreaderTrack {
    std::uint64_t seed{0};
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
            st.seed = 14695981039346656037ULL;
            const auto identity = obj.id + ":" + track.track_uid + ":" + std::to_string(channel_index);
            for (const char value : identity) {
                st.seed ^= static_cast<unsigned char>(value);
                st.seed *= 1099511628211ULL;
            }
            st.object_gain = obj.gain;
            for (const auto& blk : track.blocks) {
                const auto prepared =
                    render_common::prepare_object_block(blk, obj, lock_speakers, logs, "binaural", screen_ref_warned);
                SpreaderBlock sb;
                sb.start_sample = prepared.start_sample;
                sb.end_sample = prepared.end_sample;
                sb.head_locked = blk.head_locked;
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
        const auto [az, el] = block_position(block);
        return {{az, el, source_gain, k_binaural_extent_center_slot}};
    }
    const auto& p = block.position;
    const std::array input{p.cartesian ? p.x : p.azimuth,
                           p.cartesian ? p.y : p.elevation,
                           p.cartesian ? p.z : p.distance,
                           block.width,
                           block.height,
                           block.depth,
                           source_gain};
    std::array<MradmSceneCloudPoint, 17> points{};
    size_t count = 0;
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    std::array<float, 238> trace{};
    float* trace_data = trace.data();
    const size_t trace_size = trace.size();
#else
    float* trace_data = nullptr;
    const size_t trace_size = 0;
#endif
    dsp::scene_check(mradm_dsp_scene_cloud(input.data(),
                                           input.size(),
                                           p.cartesian ? 1U : 0U,
                                           1U | dsp::scene_arithmetic_flags,
                                           points.data(),
                                           points.size(),
                                           &count,
                                           trace_data,
                                           trace_size));
    std::vector<ExtentSource> sources;
    sources.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const auto& point = points.at(index);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
        if (count > 1) {
            consistency::dump("cloud.01-slot-" + std::to_string(point.slot) + ".f32",
                              std::span<const float>{trace}.subspan(index * 14, 14));
            consistency::dump("cloud.02-grid-" + std::to_string(point.slot) + ".i32",
                              {vbap_grid_idx(point.azimuth, point.elevation)});
        }
#endif
        sources.push_back({point.azimuth, point.elevation, point.weight, point.slot});
    }
    return sources;
}

struct BinauralSourceBank {
    std::array<BinauralSource, k_binaural_divergence_slots * k_binaural_extent_slots> direct;
    std::array<BinauralSource, k_binaural_divergence_slots * k_binaural_extent_slots> diffuse;
};

void init_source_bank(BinauralSourceBank& bank,
                      uint16_t channel_index,
                      float object_gain,
                      const std::string& object_id) {
    for (auto& src : bank.direct) {
        src.channel_index = channel_index;
        src.gain = object_gain;
        src.object_id = object_id;
        src.smoothable_object = true;
        src.diffuse_bus = false;
    }
    for (auto& src : bank.diffuse) {
        src.channel_index = channel_index;
        src.gain = object_gain;
        src.object_id = object_id;
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
    init_source_bank(track_sources, channel_index, obj.gain, obj.id);

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
                                                                    prepared.interp_length_samples,
                                                                    source_block.head_locked});
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
                                                                 prepared.interp_length_samples,
                                                                 source_block.head_locked});
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
                // Per-channel live-gain key: the channel's normalized speaker label (empty → whole-object).
                const std::string ds_label_key =
                    ds.speaker_labels.empty() ? std::string{}
                                              : render_common::canonicalise_speaker_label(ds.speaker_labels.front());
                const bool is_lfe = render_common::direct_speakers_block_is_lfe(ds);
                if (is_lfe) {
                    BinauralSource src;
                    src.channel_index = channel_index;
                    src.gain = obj.gain;
                    src.object_id = obj.id;
                    src.speaker_label_key = ds_label_key;
                    src.bypass_lfe = true;
                    src.blocks.push_back({0.0F,
                                          0.0F,
                                          ds.gain,
                                          ds.start_sample,
                                          std::min(ds.end_sample, obj.end_sample),
                                          true,
                                          std::nullopt,
                                          ds.head_locked});
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
                src.object_id = obj.id;
                src.speaker_label_key = ds_label_key;
                src.blocks.push_back({az,
                                      el,
                                      ds.gain,
                                      ds.start_sample,
                                      std::min(ds.end_sample, obj.end_sample),
                                      true,
                                      std::nullopt,
                                      ds.head_locked});
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

// A single ADM block that fully covers [start, end) with no transition or silence gap (the common
// case for beds and static objects). Returns nullptr when the chunk spans a block boundary or has
// leading/trailing silence, in which case the caller falls back to the per-segment path.
const BinauralSource::Block* steady_block_over(const BinauralSource& src, uint64_t start, uint64_t end) {
    const auto* first = first_overlapping_block(src, start, end);
    const auto* last = last_overlapping_block(src, start, end);
    if (first != nullptr && first == last && first->start_sample <= start && first->end_sample >= end) {
        return first;
    }
    return nullptr;
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

// Render one source's OLA (point / diffuse / LFE-bypass) contribution for the chunk
// [chunk_start, chunk_start + frames_now) into cs.l_out / cs.r_out (zeroed here for
// non-empty sources; empty-block sources leave the buffers untouched — they stay zero
// from construction). Carries Rust convolution / diffuse state and the HRTF cache
// across calls. The offline batch path and the realtime BinauralStream share this
// implementation and cannot drift (bit-exactness contract).
// NOLINTNEXTLINE(readability-function-size)
void render_source_ola_block(const BinauralSource& src,
                             const BinauralState& bs,
                             PerSourceConvState& cs,
                             dsp::DiffuseDelay& diffuse,
                             HrtfCache& hrtf_cache,
                             const float* in_block,
                             uint16_t num_in_ch,
                             uint64_t chunk_start,
                             uint64_t frames_now,
                             uint32_t object_smoothing_frames,
                             const HeadRotation* head = nullptr,
                             std::optional<bool> live_head_locked = std::nullopt,
                             HeadSmoothState* head_prev = nullptr) {
    if (src.blocks.empty()) {
        return;
    }

    const auto fn = static_cast<std::size_t>(frames_now);
    float* src_l = cs.l_out.data();
    float* src_r = cs.r_out.data();
    std::fill_n(src_l, fn, 0.0F);
    std::fill_n(src_r, fn, 0.0F);

    const uint64_t chunk_end = chunk_start + frames_now;

    // Head tracking is resolved per active ADM block. An explicit live value wins;
    // otherwise the block's imported effective headLocked value is used.
    const auto block_tracks_head = [&](const BinauralSource::Block& block) {
        return head != nullptr && !live_head_locked.value_or(block.head_locked);
    };
    const auto hrtf_dir = [&](const BinauralSource::Block& block, float az, float el) -> std::pair<float, float> {
        return block_tracks_head(block) ? head->rotate_az_el(az, el) : std::pair<float, float>{az, el};
    };

    // Head-rotation smoothing for the steady single-block case (beds, static objects, diffuse): as
    // the listener turns, the rotated direction shifts every block; switching the HRTF kernel abruptly
    // clicks (zipper noise). When the direction actually changed since the last block, crossfade the
    // kernel old→new across the block (object-motion smoothing below already morphs moving objects).
    const auto* steady = steady_block_over(src, chunk_start, chunk_end);
    if (head_prev != nullptr && !src.bypass_lfe && steady != nullptr && block_tracks_head(*steady)) {
        const auto [cur_az, cur_el] = head->rotate_az_el(steady->az, steady->el);
        copy_windowed_object_input(src, in_block, num_in_ch, chunk_start, frames_now, cs.ch_in.data());
        const float* conv_in = cs.ch_in.data();
        if (src.diffuse_bus) {
            diffuse.process(std::span{cs.ch_in}.first(fn), std::span{cs.diffuse_in}.first(fn));
            conv_in = cs.diffuse_in.data();
        }
        const float gain = src.gain * steady->block_gain;
        if (head_prev->valid && (cur_az != head_prev->az || cur_el != head_prev->el)) {
            const auto& start_hrtf = get_cached_hrtf(bs, head_prev->az, head_prev->el, hrtf_cache);
            const auto end_hrtf = hrtf_for_dir(bs, cur_az, cur_el);
            cs.convolver.crossfade({conv_in, fn}, start_hrtf, gain, end_hrtf, gain, {src_l, fn}, {src_r, fn});
        } else {
            const auto& hrtf = get_cached_hrtf(bs, cur_az, cur_el, hrtf_cache);
            cs.convolver.process({conv_in, fn}, hrtf, gain, {src_l, fn}, {src_r, fn});
        }
        head_prev->az = cur_az;
        head_prev->el = cur_el;
        head_prev->valid = true;
        return;
    }
    if (head_prev != nullptr) {
        // Tracking inactive/head-locked, or a block boundary/gap: re-seed if a
        // later steady world-relative block starts tracking again.
        head_prev->valid = false;
    }

    if (object_smoothing_frames > 0 && src.smoothable_object && !src.bypass_lfe) {
        const auto* start_block = first_overlapping_block(src, chunk_start, chunk_end);
        const auto* end_block = last_overlapping_block(src, chunk_start, chunk_end);
        if (start_block != nullptr && end_block != nullptr && start_block != end_block && !end_block->jump_position) {
            const uint64_t interp_len = end_block->interp_length_samples.value_or(object_smoothing_frames);
            if (interp_len > 0U) {
                copy_windowed_object_input(src, in_block, num_in_ch, chunk_start, frames_now, cs.ch_in.data());
                const float* conv_in = cs.ch_in.data();
                if (src.diffuse_bus) {
                    diffuse.process(std::span{cs.ch_in}.first(fn), std::span{cs.diffuse_in}.first(fn));
                    conv_in = cs.diffuse_in.data();
                }
                const auto [s_az, s_el] = hrtf_dir(*start_block, start_block->az, start_block->el);
                const auto [e_az, e_el] = hrtf_dir(*end_block, end_block->az, end_block->el);
                const auto& start_hrtf = get_cached_hrtf(bs, s_az, s_el, hrtf_cache);
                const auto end_hrtf = hrtf_for_dir(bs, e_az, e_el);
                cs.convolver.crossfade({conv_in, fn},
                                       start_hrtf,
                                       src.gain * start_block->block_gain,
                                       end_hrtf,
                                       src.gain * end_block->block_gain,
                                       {src_l, fn},
                                       {src_r, fn});
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
            cs.convolver.advance_silence({src_l + off, static_cast<std::size_t>(chunk_end - cursor)},
                                         {src_r + off, static_cast<std::size_t>(chunk_end - cursor)});
            break;
        }

        const auto& blk = src.blocks[bi];
        if (cursor < blk.start_sample) {
            const uint64_t silent_end = std::min<uint64_t>(blk.start_sample, chunk_end);
            const auto off = static_cast<std::size_t>(cursor - chunk_start);
            cs.convolver.advance_silence({src_l + off, static_cast<std::size_t>(silent_end - cursor)},
                                         {src_r + off, static_cast<std::size_t>(silent_end - cursor)});
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
                diffuse.process(std::span{cs.ch_in}.first(seg_fn), std::span{cs.diffuse_in}.first(seg_fn));
                conv_in = cs.diffuse_in.data();
            }
            const auto [h_az, h_el] = hrtf_dir(blk, blk.az, blk.el);
            const auto& hrtf = get_cached_hrtf(bs, h_az, h_el, hrtf_cache);
            cs.convolver.process({conv_in, seg_fn}, hrtf, gain, {src_l + off, seg_fn}, {src_r + off, seg_fn});
        }

        cursor = seg_end;
        if (cursor >= blk.end_sample) {
            ++bi;
        }
    }
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

// Realtime streaming binaural session over the same prepared HRTF tables + source list as
// render_window. It renders render_block_size-aligned blocks on demand into a FIFO, from
// which process() serves any requested frame count via render_source_ola_block — the SAME
// per-source OLA path render_window uses — so a gap-free run from frame 0 is bit-identical
// to the offline render (the smoke test asserts this). saf_spreader mode (STFT latency
// compensation) is not yet streamable and is rejected at open. seek() resets the per-source
// OLA / diffuse / HRTF-cache state (a small discontinuity, acceptable for monitoring) and
// repositions the reader. set_overrides drives per-source sample gain ramps and coalesced
// topology state crossfades. The expensive prepared state is borrowed by reference; the owning
// factory keeps it alive for the stream's lifetime.
class BinauralStream final : public IRenderStream {
  public:
    [[nodiscard]] static Result<std::unique_ptr<BinauralStream>>
    create(const BinauralPrepared& prepared, const RenderPlan& plan, LogSink& logs) {
        if (plan.binaural_spread_mode == BinauralSpreadMode::saf_spreader && !prepared.spreader_groups.empty()) {
            return make_error(ErrorCode::unsupported,
                              "binaural realtime stream does not yet support saf_spreader mode (use cloud / none)",
                              {});
        }
        const auto& info = plan.scene.info;
        if (info.sample_rate != 48000U) {
            return make_error(ErrorCode::invalid_argument,
                              fmt::format("binaural renderer requires 48000 Hz input, got {}", info.sample_rate),
                              "input=" + plan.input_path);
        }
        auto reader = audio::RenderInputReader::open(plan.input_path);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        (void) logs;
        return std::unique_ptr<BinauralStream>{new BinauralStream(prepared,
                                                                  std::move(*reader),
                                                                  plan.scene,
                                                                  plan.binaural_spread_mode,
                                                                  info.num_channels,
                                                                  info.num_frames,
                                                                  info.sample_rate,
                                                                  plan.object_smoothing_frames)};
    }

    ~BinauralStream() override = default;
    BinauralStream(const BinauralStream&) = delete;
    BinauralStream& operator=(const BinauralStream&) = delete;
    BinauralStream(BinauralStream&&) = delete;
    BinauralStream& operator=(BinauralStream&&) = delete;

    [[nodiscard]] Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        std::size_t produced = 0;
        while (produced < frames) {
            if (fifo_read_ >= fifo_.size()) {
                if (frames_done_ >= total_frames_) {
                    break; // end of material
                }
                if (auto result = render_block(); !result) {
                    return tl::unexpected{result.error()};
                }
                if (fifo_read_ >= fifo_.size()) {
                    break;
                }
            }
            const std::size_t avail = (fifo_.size() - fifo_read_) / 2U;
            const std::size_t take = std::min(frames - produced, avail);
            std::copy_n(fifo_.data() + fifo_read_, take * 2U, out.data() + (produced * 2U));
            fifo_read_ += take * 2U;
            produced += take;
        }
        return produced;
    }

    [[nodiscard]] Result<void> seek(uint64_t frame) override {
        reset_dsp_state(); // small discontinuity (OLA / diffuse tails dropped); fine for monitoring
        frames_done_ = std::min(frame, total_frames_);
        if (auto result = reader_->seek_frame(frames_done_); !result) {
            return tl::unexpected{result.error()};
        }
        fifo_.clear();
        fifo_read_ = 0;
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        // Gain targets are resolved per semantic source key. The render block builds one shared
        // sample-domain envelope per key, so every spatial component of an object follows the same
        // de-zippered trajectory (including per-channel DirectSpeakers overrides).
        live_overrides_ = overrides;
        update_live_gain_targets();

        // Topology changes need a new source graph and fresh convolution state. Queue the newest
        // graph while dragging; render_block applies at most one every 100 ms and crossfades old
        // and new states over the same source block. This preserves the old OLA/diffuse tail at the
        // boundary and prevents a 60 Hz UI gesture from repeatedly destroying the DSP state.
        std::unordered_map<std::string, LiveTopologyScales> topo;
        for (const auto& ov : overrides.objects) {
            const LiveTopologyScales scales{
                ov.diffuse_scale,
                ov.extent_scale * ov.extent_width_scale,
                ov.extent_scale * ov.extent_height_scale,
                ov.extent_scale * ov.extent_depth_scale,
                ov.divergence_scale,
            };
            if (scales[k_topology_diffuse] != 1.0F || scales[k_topology_width] != 1.0F ||
                scales[k_topology_height] != 1.0F || scales[k_topology_depth] != 1.0F ||
                scales[k_topology_divergence] != 1.0F) {
                topo[ov.object_id] = scales;
            }
        }
        pending_topology_ = std::move(topo);
        topology_pending_ = pending_topology_ != applied_topology_;
    }

    // Live listener head orientation (head tracking / free-look). Applied at the worker render
    // stage: world-locked sources are rotated into the head frame before the HRTF lookup (cheap —
    // it only changes the per-source HRTF direction; no re-prepare). Head-locked sources ignore it.
    // The MonitorEngine's shallow render-ahead while tracking keeps the latency low (the SAF OLA
    // convolution is too heavy/stateful for the audio callback, so there is no output-stage path).
    void set_listener_orientation(const ListenerOrientation& orientation) override {
        listener_orientation_ = orientation;
    }

    [[nodiscard]] uint32_t out_channels() const override { return 2U; }
    [[nodiscard]] uint32_t sample_rate() const override { return sample_rate_; }
    [[nodiscard]] std::string_view output_layout() const override { return "binaural"; }

  private:
    BinauralStream(const BinauralPrepared& prepared,
                   std::unique_ptr<audio::RenderInputReader> reader,
                   AdmScene scene,
                   BinauralSpreadMode spread_mode,
                   uint16_t num_in_ch,
                   uint64_t total_frames,
                   uint32_t sample_rate,
                   uint32_t object_smoothing_frames)
        : prepared_(prepared), reader_(std::move(reader)), scene_(std::move(scene)), spread_mode_(spread_mode),
          sources_(prepared.sources), num_in_ch_(num_in_ch), total_frames_(total_frames), sample_rate_(sample_rate),
          object_smoothing_frames_(object_smoothing_frames), render_block_size_(prepared.render_block_size),
          ola_pool_(ola_worker_count(prepared.sources.size())),
          in_block_(static_cast<std::size_t>(num_in_ch) * prepared.render_block_size, 0.0F),
          topology_update_interval_frames_(
              std::max<uint64_t>(1U, (static_cast<uint64_t>(sample_rate) * k_live_topology_min_interval_ms) / 1000U)) {
        // sources_ starts as a copy of the prepared list (== build_sources(scene_)); a
        // topology override rebuilds it from a scaled scene_. bs (HRTF/VBAP) is never rebuilt.
        init_per_source_state();
    }

    [[nodiscard]] static std::size_t ola_worker_count(std::size_t num_sources) {
        const auto hw_threads = static_cast<std::size_t>(std::thread::hardware_concurrency());
        // A point source can expand into many extent/divergence sources after a live topology edit.
        // Keep workers ready for that graph instead of sizing the pool only to the initial point graph.
        return (num_sources > 0U && hw_threads > 1U) ? hw_threads : 0U;
    }

    class LiveGainSlot {
      public:
        LiveGainSlot(std::string object, std::string speaker_label, uint32_t sample_rate, std::size_t block_frames)
            : object_id(std::move(object)), speaker_label_key(std::move(speaker_label)), ramp(sample_rate),
              envelope(block_frames, 1.0F) {}

        [[nodiscard]] const std::string& object() const noexcept { return object_id; }
        [[nodiscard]] const std::string& speaker_label() const noexcept { return speaker_label_key; }
        [[nodiscard]] render_common::LiveGainRamp& gain_ramp() noexcept { return ramp; }
        [[nodiscard]] std::vector<float>& gain_envelope() noexcept { return envelope; }
        [[nodiscard]] const std::vector<float>& gain_envelope() const noexcept { return envelope; }

      private:
        std::string object_id;
        std::string speaker_label_key;
        render_common::LiveGainRamp ramp;
        std::vector<float> envelope;
    };

    [[nodiscard]] static std::string live_gain_key(const BinauralSource& source) {
        std::string key = source.object_id;
        key.push_back('\0');
        key += source.speaker_label_key;
        return key;
    }

    void assign_live_gain_slots() {
        source_gain_slots_.clear();
        source_gain_slots_.reserve(sources_.size());
        for (const auto& source : sources_) {
            const std::string key = live_gain_key(source);
            const auto found = gain_slot_by_key_.find(key);
            if (found != gain_slot_by_key_.end()) {
                source_gain_slots_.push_back(found->second);
                continue;
            }
            const std::size_t slot = live_gain_slots_.size();
            gain_slot_by_key_.emplace(key, slot);
            live_gain_slots_.emplace_back(
                source.object_id, source.speaker_label_key, sample_rate_, static_cast<std::size_t>(render_block_size_));
            source_gain_slots_.push_back(slot);
        }
        update_live_gain_targets();
    }

    void update_live_gain_targets() {
        for (auto& slot : live_gain_slots_) {
            const float target =
                render_common::resolve_live_channel_gain(live_overrides_, slot.object(), slot.speaker_label())
                    .value_or(1.0F);
            slot.gain_ramp().set_target(target);
        }
    }

    void prepare_live_gain_envelopes(std::size_t frames) {
        for (auto& slot : live_gain_slots_) {
            slot.gain_ramp().fill(std::span{slot.gain_envelope()}.first(frames));
        }
    }

    // (Re)create the per-source FFT plans + scratch sized to sources_, then reset the
    // cross-call DSP state. Used at construction and after a topology rebuild.
    void init_per_source_state() {
        const auto& bs = *prepared_.bs;
        src_cs_.clear();
        src_cs_.resize(sources_.size());
        for (auto& cs : src_cs_) {
            cs.convolver = dsp::OlaConvolver(static_cast<std::size_t>(bs.fft_size),
                                             static_cast<std::size_t>(bs.overlap_len),
                                             static_cast<std::size_t>(render_block_size_));
            cs.l_out.resize(static_cast<std::size_t>(render_block_size_));
            cs.r_out.resize(static_cast<std::size_t>(render_block_size_));
            cs.ch_in.resize(static_cast<std::size_t>(render_block_size_));
            cs.diffuse_in.resize(static_cast<std::size_t>(render_block_size_));
        }
        reset_dsp_state();
        assign_live_gain_slots();
    }

    // Reset Rust signal history while retaining FFT plans and prepared storage.
    // Used at construction, on seek, and after a topology rebuild.
    void reset_dsp_state() {
        const std::size_t n = sources_.size();
        for (auto& cs : src_cs_) {
            cs.convolver.reset();
        }
        diffuse_delay_.resize(n);
        for (auto& delay : diffuse_delay_) {
            delay.reset();
        }
        hrtf_cache_.assign(n, HrtfCache{});
        head_smooth_.assign(n, HeadSmoothState{});
    }

    // Cheap re-prepare: rebuild sources_ from a copy of the scene with each overridden
    // object's block diffuse / extent (width,height,depth) / divergence multiplied by its
    // scale (clamped to ADM's 0..1), then re-init per-source state. Empty `topo` rebuilds
    // the unscaled scene, which reproduces the prepared list bit-for-bit.
    void rebuild_sources(const std::unordered_map<std::string, LiveTopologyScales>& topo) {
        AdmScene scaled = scene_;
        for (auto& obj : scaled.objects) {
            const auto it = topo.find(obj.id);
            if (it == topo.end()) {
                continue;
            }
            const auto& s = it->second;
            for (auto& track : obj.tracks) {
                for (auto& blk : track.blocks) {
                    blk.diffuse = std::clamp(blk.diffuse * s[k_topology_diffuse], 0.0F, 1.0F);
                    blk.width = std::clamp(blk.width * s[k_topology_width], 0.0F, 1.0F);
                    blk.height = std::clamp(blk.height * s[k_topology_height], 0.0F, 1.0F);
                    blk.depth = std::clamp(blk.depth * s[k_topology_depth], 0.0F, 1.0F);
                    blk.divergence = std::clamp(blk.divergence * s[k_topology_divergence], 0.0F, 1.0F);
                }
            }
        }
        NullLogSink null_logs;
        sources_ = build_sources(scaled, null_logs, spread_mode_);
        init_per_source_state();
    }

    void render_current_sources(std::vector<float>& output, uint64_t frames_now) {
        const auto fn = static_cast<std::size_t>(frames_now);
        const auto& sources = sources_;
        // Live head tracking: rotate world-locked sources into the head frame at render time (worker
        // stage). Built once per block; head-locked sources (per-object override) keep their raw
        // direction. Identity pose ⇒ no rotation object, output stays bit-identical to no-tracking.
        const bool head_active = !listener_orientation_.is_identity();
        head_rotation_.update(listener_orientation_);
        ola_pool_.parallel_for(sources.size(), [&](std::size_t si) {
            const auto live_head_locked = render_common::resolve_live_head_locked(
                live_overrides_, sources[si].object_id, sources[si].speaker_label_key);
            const HeadRotation* head = head_active ? &head_rotation_ : nullptr;
            render_source_ola_block(sources[si],
                                    *prepared_.bs,
                                    src_cs_[si],
                                    diffuse_delay_[si],
                                    hrtf_cache_[si],
                                    in_block_.data(),
                                    num_in_ch_,
                                    frames_done_,
                                    frames_now,
                                    object_smoothing_frames_,
                                    head,
                                    live_head_locked,
                                    &head_smooth_[si]);
        });

        // Reduce per-source L/R into the interleaved FIFO, applying the live per-object
        // gain. With no overrides every gain is 1.0 and the summation order matches
        // render_window's reduce, so the output is bit-identical to the offline render.
        output.assign(fn * 2U, 0.0F);
        for (std::size_t si = 0; si < sources.size(); ++si) {
            const auto& gain = live_gain_slots_[source_gain_slots_[si]].gain_envelope();
            const float* sl = src_cs_[si].l_out.data();
            const float* sr = src_cs_[si].r_out.data();
            for (std::size_t f = 0; f < fn; ++f) {
                output[(f * 2U) + 0U] += sl[f] * gain[f];
                output[(f * 2U) + 1U] += sr[f] * gain[f];
            }
        }
    }

    Result<void> render_block() {
        const uint64_t frames_now = std::min<uint64_t>(render_block_size_, total_frames_ - frames_done_);
        const auto fn = static_cast<std::size_t>(frames_now);
        const auto read_result = reader_->read(in_block_.data(), frames_now);
        if (!read_result) {
            return tl::unexpected{read_result.error()};
        }
        if (*read_result != frames_now) {
            return make_error(ErrorCode::io_error, "short render input read");
        }
        // Before the first sample there is no outgoing state to preserve. Apply the coalesced initial
        // topology directly, then begin gain envelopes on the final source graph.
        if (frames_done_ == 0U && topology_pending_) {
            rebuild_sources(pending_topology_);
            applied_topology_ = pending_topology_;
            topology_pending_ = false;
        }
        prepare_live_gain_envelopes(fn);

        const bool apply_topology = topology_pending_ && frames_done_ >= next_topology_update_frame_;
        if (apply_topology) {
            render_current_sources(old_fifo_, frames_now);
            rebuild_sources(pending_topology_);
            render_current_sources(fifo_, frames_now);
            for (std::size_t frame = 0; frame < fn; ++frame) {
                const float mix = fn <= 1U ? 1.0F : static_cast<float>(frame) / static_cast<float>(fn - 1U);
                for (std::size_t channel = 0; channel < 2U; ++channel) {
                    const std::size_t index = (frame * 2U) + channel;
                    fifo_[index] = (old_fifo_[index] * (1.0F - mix)) + (fifo_[index] * mix);
                }
            }
            applied_topology_ = pending_topology_;
            topology_pending_ = false;
            next_topology_update_frame_ = frames_done_ + topology_update_interval_frames_;
        } else {
            render_current_sources(fifo_, frames_now);
        }
        fifo_read_ = 0;
        frames_done_ += frames_now;
        return {};
    }

    const BinauralPrepared& prepared_; // borrowed; owner (factory) outlives the stream
    std::unique_ptr<audio::RenderInputReader> reader_;
    // policy-applied scene, for topology re-prepare (scaled copy → build_sources)
    AdmScene scene_;
    BinauralSpreadMode spread_mode_;      // spread mode the prepared sources were built with
    std::vector<BinauralSource> sources_; // own (rebuildable) source list; starts == prepared_.sources
    uint16_t num_in_ch_;
    uint64_t total_frames_;
    uint32_t sample_rate_;
    uint32_t object_smoothing_frames_;
    uint64_t render_block_size_;

    std::vector<dsp::DiffuseDelay> diffuse_delay_;
    std::vector<HrtfCache> hrtf_cache_;
    std::vector<HeadSmoothState> head_smooth_; // per-source last head-rotated dir (head-tracking zipper guard)
    std::vector<PerSourceConvState> src_cs_;
    TrackWorkerPool ola_pool_;
    std::vector<float> in_block_;
    LiveOverrides live_overrides_; // latest snapshot; gain targets are projected into live_gain_slots_
    std::unordered_map<std::string, std::size_t> gain_slot_by_key_;
    std::vector<LiveGainSlot> live_gain_slots_;
    std::vector<std::size_t> source_gain_slots_; // sources_ index → live_gain_slots_ index
    HeadRotation head_rotation_{ListenerOrientation{}};
    ListenerOrientation listener_orientation_{}; // live head pose (worker-only; identity = no tracking)
    // object_id → non-unity topology scales; only non-unity entries.
    // The last applied topology, so set_overrides rebuilds only when it actually changes.
    std::unordered_map<std::string, LiveTopologyScales> applied_topology_;
    std::unordered_map<std::string, LiveTopologyScales> pending_topology_;
    bool topology_pending_{false};
    uint64_t topology_update_interval_frames_{1U};
    uint64_t next_topology_update_frame_{0U};

    std::vector<float> fifo_;     // interleaved L/R produced, not yet served
    std::vector<float> old_fifo_; // one block of the outgoing topology during a state crossfade
    std::size_t fifo_read_{0};
    uint64_t frames_done_{0};
};

class BinauralRenderer final : public IRenderer {
  public:
    [[nodiscard]] CapabilityReport capabilities() const override;
    [[nodiscard]] Result<std::shared_ptr<IPreparedRender>> prepare(const RenderPlan& plan, LogSink& logs) override;
    [[nodiscard]] Result<RenderMetrics> render_window(const IPreparedRender& prepared,
                                                      const RenderPlan& plan,
                                                      ProgressSink& progress,
                                                      LogSink& logs) override;

    [[nodiscard]] Result<std::unique_ptr<IRenderStream>>
    open_stream(const IPreparedRender& prep, const RenderPlan& plan, LogSink& logs) override {
        const auto* prepared = dynamic_cast<const BinauralPrepared*>(&prep);
        if (prepared == nullptr) {
            return make_error(
                ErrorCode::internal_error, "binaural: open_stream received an incompatible prepared state", {});
        }
        auto stream = BinauralStream::create(*prepared, plan, logs);
        if (!stream) {
            return tl::unexpected{stream.error()};
        }
        return std::unique_ptr<IRenderStream>{std::move(*stream)};
    }
};

CapabilityReport BinauralRenderer::capabilities() const {
    return binaural_capabilities();
}

Result<std::shared_ptr<IPreparedRender>> BinauralRenderer::prepare(const RenderPlan& plan, LogSink& logs) {
    if (plan.direct_speakers_routing_mode != DirectSpeakersRoutingMode::automatic) {
        return make_error(ErrorCode::unsupported,
                          "SAF binaural renderer does not support explicit DirectSpeakers routing; use automatic",
                          {});
    }
    auto lfe_routing = render_common::resolve_lfe_routing(plan, logs, "binaural");
    if (!lfe_routing) {
        return tl::unexpected{lfe_routing.error()};
    }
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
    auto state_result = build_binaural_state(std::move(*dataset_res), render_block_size);
    if (!state_result) {
        return tl::unexpected{state_result.error()};
    }
    auto bs = std::move(*state_result);
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
        const std::size_t parallel_budget = consistency::count_override(
            "MR_ADM_DIAGNOSTIC_GROUP_BUDGET", (hw_threads > 0U) ? hw_threads : sources.size());
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
            std::vector<std::uint64_t> seeds;
            seeds.reserve(group.lanes.size());
            for (const auto& lane : group.lanes) {
                const auto& track = spreader_tracks[group.track_indices[lane.track_slot]];
                seeds.push_back(track.seed ^ ((lane.source_index + 1U) * 0x9e3779b97f4a7c15ULL));
            }
            return BinauralSpreaderAdapter{bs->hrtf_td.data(),
                                           bs->grid_dirs_deg.data(),
                                           bs->num_dirs,
                                           bs->hrir_len,
                                           bs->sample_rate,
                                           static_cast<int>(group.lanes.size()),
                                           seeds};
        });
        // Prime each adapter: pre-fill the output ring with one frame so process_chunk
        // drains exactly n_frames (constant total_latency(), no gaps).
        for (auto& adapter : spreader_adapters) {
            adapter.prime();
        }
    }

    // Open I/O.
    auto reader_res = audio::RenderInputReader::open(plan.input_path);
    if (!reader_res) {
        return tl::unexpected{reader_res.error()};
    }
    auto reader = std::move(*reader_res);
    auto writer_res = audio::WriterHandle::open(plan.output_path, 2U, info.sample_rate, "binaural");
    if (!writer_res) {
        return tl::unexpected{writer_res.error()};
    }
    auto& writer = *writer_res;

    // Inline loudness + true-peak measurement.
    auto lufs_st =
        dsp::Meter::create(2U, static_cast<uint32_t>(info.sample_rate), dsp::MeterMode::integrated_true_peak);
    if (!lufs_st) {
        return tl::unexpected{lufs_st.error()};
    }

    std::vector<dsp::DiffuseDelay> diffuse_delay(sources.size());
    std::vector<HrtfCache> hrtf_cache(sources.size());
    std::vector<HeadSmoothState> head_smooth(sources.size());

    // Per-source FFT handles, scratch, and output buffers for parallel OLA processing.
    std::vector<PerSourceConvState> src_cs(sources.size());
    for (auto& cs : src_cs) {
        cs.convolver = dsp::OlaConvolver(static_cast<std::size_t>(bs->fft_size),
                                         static_cast<std::size_t>(bs->overlap_len),
                                         static_cast<std::size_t>(render_block_size));
        cs.l_out.resize(static_cast<std::size_t>(render_block_size));
        cs.r_out.resize(static_cast<std::size_t>(render_block_size));
        cs.ch_in.resize(static_cast<std::size_t>(render_block_size));
        cs.diffuse_in.resize(static_cast<std::size_t>(render_block_size));
    }
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
    const std::size_t spreader_worker_count = consistency::count_override(
        "MR_ADM_DIAGNOSTIC_WORKERS",
        (spreader_groups.size() > 1U && hw_threads > 1U) ? std::min(spreader_groups.size(), hw_threads) : 0U);
    TrackWorkerPool spreader_pool(spreader_worker_count);
    const std::size_t ola_worker_count = consistency::count_override(
        "MR_ADM_DIAGNOSTIC_WORKERS",
        (sources.size() > 1U && hw_threads > 1U) ? std::min(sources.size(), hw_threads) : 0U);
    TrackWorkerPool ola_pool(ola_worker_count);
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    logs.log(LogLevel::info,
             "binaural",
             fmt::format("diagnostic pools: ola_workers={} spreader_workers={} ola_sources={} spreader_groups={}",
                         ola_worker_count,
                         spreader_worker_count,
                         sources.size(),
                         spreader_groups.size()));
#endif
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
        if (auto result = reader->seek_frame(start_pos); !result) {
            return tl::unexpected{result.error()};
        }
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
            if (const auto result = lufs_st->add_frames(out_block.data(), want); !result) {
                throw std::runtime_error(result.error().message);
            }
        }
        if (writer.write(out_block.data(), want) != want) {
            return false;
        }
        out_written += want;
        return true;
    };

    progress.on_progress({RenderStage::rendering, RenderOperation::render_audio, 0.2, 0.0, 0, 0, "rendering"});

    // Static listener orientation (offline head pose): rotates every source into the head frame.
    const bool head_pose_active = !plan.listener_orientation.is_identity();
    const HeadRotation offline_head_rot{plan.listener_orientation};

    while (frames_done < num_frames && out_abs < win_end) {
        if (plan.cancel_token.stop_requested()) {
            return make_error(ErrorCode::cancelled, "render cancelled", "output=" + plan.output_path);
        }
        const uint64_t frames_now = std::min(render_block_size, num_frames - frames_done);
        const auto fn = static_cast<std::size_t>(frames_now);

        const auto read_result = reader->read(in_block.data(), frames_now);
        if (!read_result) {
            return tl::unexpected{read_result.error()};
        }
        if (*read_result != frames_now) {
            return make_error(ErrorCode::io_error, "short render input read");
        }
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

        // Process each OLA source independently; parallelise across sources. A non-identity static
        // listener orientation (plan.listener_orientation) rotates each scene-relative source into
        // the head frame. The active ADM block's effective headLocked value exempts head-relative
        // sources; the offline path simply has no additional live override layer.
        const HeadRotation* offline_head = head_pose_active ? &offline_head_rot : nullptr;
        ola_pool.parallel_for(sources.size(), [&](std::size_t si) {
            render_source_ola_block(sources[si],
                                    *bs,
                                    src_cs[si],
                                    diffuse_delay[si],
                                    hrtf_cache[si],
                                    in_block.data(),
                                    num_in_ch,
                                    frames_done,
                                    frames_now,
                                    plan.object_smoothing_frames,
                                    offline_head,
                                    std::nullopt,
                                    &head_smooth[si]);
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
                            // Rotate the spreader cone's centre direction by the static listener pose too,
                            // so the extent cloud tracks the head consistently with the point-source path
                            // above (the cone half-angle spread_deg is orientation-independent).
                            const auto [sp_az, sp_el] = offline_head != nullptr && !active->head_locked
                                                            ? offline_head->rotate_az_el(ss.az, ss.el)
                                                            : std::pair<float, float>{ss.az, ss.el};
                            adapter.set_source(
                                static_cast<int>(li), sp_az, sp_el, ss.spread_deg, st.object_gain * ss.gain);
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
        if (const auto loudness = lufs_st->integrated(); loudness && std::isfinite(*loudness)) {
            metrics.measured_lufs = *loudness;
        }
        if (const auto peak = lufs_st->max_true_peak(); peak) {
            // Preserve the binaural backend's established -200 dB silent floor.
            metrics.measured_peak_dbtp = 20.0 * std::log10(std::max(*peak, 1e-10));
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
    r.hrtf_sources = {"built-in"};
#if MR_ADM_ENABLE_SOFA
    r.hrtf_sources.emplace_back("user-sofa");
#endif
    // Default (cloud/none) path windows via seek + aligned pre-roll; the experimental
    // saf_spreader path keeps full STFT warm-up and only trims output.
    r.supports_render_window = true;
    r.supported_layouts = {
        // clang-format off
        {"binaural", "SAF HRTF binaural (KEMAR or user SOFA HRIR)", 2, false, 0, true, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

bool binaural_sofa_supported() {
#if MR_ADM_ENABLE_SOFA
    return true;
#else
    return false;
#endif
}

} // namespace mradm
