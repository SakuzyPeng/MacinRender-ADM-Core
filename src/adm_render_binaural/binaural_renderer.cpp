#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ebur128.h>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <vector>
// clang-format off
// saf_utility_complex.h must precede saf_hrir.h: saf_hrir.h opens extern "C" and
// re-includes saf_utility_complex.h inside that block, causing std::complex<float>
// template instantiation in C linkage if the include guard hasn't fired yet.
#include <saf_utility_complex.h>
#include <saf_utility_fft.h>
#include <saf_hrir.h>
#include <saf_vbap.h>
// clang-format on

#include <bw64/bw64.hpp>
#include <fmt/format.h>

#include "adm/audio_io.h"
#include "adm/render.h"
#include "adm/render_binaural.h"

namespace mradm {

namespace {

// ── Constants ────────────────────────────────────────────────────────────────

constexpr uint64_t k_block_size = 1024U;
constexpr int k_hrir_len = 256;                 // __default_hrir_len
constexpr int k_fft_size = 2048;                // next pow2 >= k_block_size + k_hrir_len - 1
constexpr int k_n_bands = (k_fft_size / 2) + 1; // 1025
constexpr int k_n_hrtf_dirs = 836;              // __default_N_hrir_dirs
constexpr int k_n_ears = 2;
constexpr int k_overlap_len = k_hrir_len - 1; // 255 OLA tail samples
constexpr int k_n_azi = 361;                  // (int)(360/1 + 0.5) + 1
constexpr int k_n_elev = 181;                 // (int)(180/1 + 0.5) + 1

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

// ADM Cartesian (right=+X, front=+Y, up=+Z) → polar az/el in degrees.
std::pair<float, float> cart_to_polar(float x, float y, float z) {
    const double az =
        std::atan2(static_cast<double>(-x), static_cast<double>(y)) * (180.0 / std::numbers::pi_v<double>);
    const auto cx = static_cast<double>(x);
    const auto cy = static_cast<double>(y);
    const double el =
        std::atan2(static_cast<double>(z), std::sqrt((cx * cx) + (cy * cy))) * (180.0 / std::numbers::pi_v<double>);
    return {static_cast<float>(az), static_cast<float>(el)};
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

// ── Pre-computed state ────────────────────────────────────────────────────────

// NOLINTBEGIN(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)
struct BinauralState {
    // HRTFs in frequency domain; FLAT: [k_n_bands × k_n_ears × k_n_hrtf_dirs]
    std::vector<float_complex> hrtf_fd;
    // Compressed VBAP table: amplitude-normalised gains + direction indices per grid point.
    // Each grid point has 3 gains and 3 indices (triangular interpolation).
    std::vector<float> vbap_gains; // FLAT: [N_gtable × 3]
    std::vector<int> vbap_dirs;    // FLAT: [N_gtable × 3]

    BinauralState() = default;
    BinauralState(const BinauralState&) = delete;
    BinauralState& operator=(const BinauralState&) = delete;
};
// NOLINTEND(cppcoreguidelines-special-member-functions,misc-non-private-member-variables-in-classes)

// Build BinauralState once.  Returns nullptr on VBAP triangulation failure.
std::unique_ptr<BinauralState> build_binaural_state() {
    auto bs = std::make_unique<BinauralState>();

    // Convert built-in HRIRs to frequency-domain HRTFs.
    bs->hrtf_fd.resize(static_cast<std::size_t>(k_n_bands) * k_n_ears * k_n_hrtf_dirs);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
    HRIRs2HRTFs(
        const_cast<float*>(&__default_hrirs[0][0][0]), k_n_hrtf_dirs, k_hrir_len, k_fft_size, bs->hrtf_fd.data());

    // Build VBAP gain table for the 836 HRTF measurement directions as
    // "loudspeakers".  1° grid; gains are energy-normalised after this call.
    auto* hrtf_dirs = const_cast<float*>(&__default_hrir_dirs_deg[0][0]);
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
    float* gtable_full = nullptr;
    int n_gtable = 0;
    int n_triangles = 0;
    generateVBAPgainTable3D(hrtf_dirs,
                            k_n_hrtf_dirs,
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
    compressVBAPgainTable3D(gtable_full, n_gtable, k_n_hrtf_dirs, bs->vbap_gains.data(), bs->vbap_dirs.data());
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    free(gtable_full);

    return bs;
}

// Interpolate HRTF at (az_deg, el_deg) using the compressed VBAP table.
// Returns flat [k_n_bands × k_n_ears] complex array (L = ear 0, R = ear 1).
std::vector<float_complex> hrtf_for_dir(const BinauralState& bs, float az_deg, float el_deg) {
    const auto g = static_cast<std::size_t>(vbap_grid_idx(az_deg, el_deg));
    const auto gbase = g * 3U;
    const float w0 = bs.vbap_gains[gbase + 0U];
    const float w1 = bs.vbap_gains[gbase + 1U];
    const float w2 = bs.vbap_gains[gbase + 2U];
    const int d0 = bs.vbap_dirs[gbase + 0U];
    const int d1 = bs.vbap_dirs[gbase + 1U];
    const int d2 = bs.vbap_dirs[gbase + 2U];

    std::vector<float_complex> out(static_cast<std::size_t>(k_n_bands) * k_n_ears);
    for (int band = 0; band < k_n_bands; ++band) {
        for (int ear = 0; ear < k_n_ears; ++ear) {
            const auto base = (static_cast<std::ptrdiff_t>(band) * k_n_ears * k_n_hrtf_dirs) +
                              (static_cast<std::ptrdiff_t>(ear) * k_n_hrtf_dirs);
            out[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)] =
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d0)], w0) +
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d1)], w1) +
                crmulf(bs.hrtf_fd[static_cast<std::size_t>(base + d2)], w2);
        }
    }
    return out;
}

// ── Per-channel OLA convolution state ─────────────────────────────────────────

struct OLAState {
    std::vector<float> overlap_l = std::vector<float>(k_overlap_len, 0.0F);
    std::vector<float> overlap_r = std::vector<float>(k_overlap_len, 0.0F);
};

// OLA convolution: convolve src[frames_now] with hrtf[k_n_bands × k_n_ears],
// accumulate into l_out/r_out (both [frames_now]), update state.overlap.
// Uses saf_rfft (KissFFT backend).
void convolve_and_accumulate(void* hfft,
                             const float* src,
                             uint64_t frames_now,
                             float gain,
                             const std::vector<float_complex>& hrtf,
                             OLAState& state,
                             float* l_out,
                             float* r_out) {
    const auto fn = static_cast<std::size_t>(frames_now);

    std::vector<float> buf(k_fft_size, 0.0F);
    std::vector<float_complex> src_fd(k_n_bands);
    std::vector<float_complex> out_fd(k_n_bands);
    std::vector<float> y(k_fft_size);

    // Zero-pad source block into FFT buffer and transform.
    std::copy_n(src, fn, buf.begin());
    saf_rfft_forward(hfft, buf.data(), src_fd.data());

    for (int ear = 0; ear < k_n_ears; ++ear) {
        // Frequency-domain multiply: src_fd × hrtf[:][ear].
        for (int band = 0; band < k_n_bands; ++band) {
            out_fd[static_cast<std::size_t>(band)] =
                gain * src_fd[static_cast<std::size_t>(band)] *
                hrtf[(static_cast<std::size_t>(band) * k_n_ears) + static_cast<std::size_t>(ear)];
        }
        saf_rfft_backward(hfft, out_fd.data(), y.data());

        float* overlap = (ear == 0) ? state.overlap_l.data() : state.overlap_r.data();
        float* dst = (ear == 0) ? l_out : r_out;

        // Overlap-add: accumulated output = y[0..fn-1] + saved overlap.
        for (std::size_t f = 0; f < fn; ++f) {
            dst[f] += y[f] + (f < static_cast<std::size_t>(k_overlap_len) ? overlap[f] : 0.0F);
        }
        // Save new overlap tail: y[fn .. fn + k_overlap_len - 1].
        for (int i = 0; i < k_overlap_len; ++i) {
            overlap[i] = y[fn + static_cast<std::size_t>(i)];
        }
    }
}

// Advance a source's FIR overlap through a silent interval.  This emits the tail
// from a previous active segment at the correct absolute time instead of carrying
// it forward to the next non-silent ADM block.
void advance_silence(OLAState& state, uint64_t frames_now, float* l_out, float* r_out) {
    const auto fn = static_cast<std::size_t>(frames_now);
    const auto emit = std::min(fn, static_cast<std::size_t>(k_overlap_len));
    for (std::size_t f = 0; f < emit; ++f) {
        l_out[f] += state.overlap_l[f];
        r_out[f] += state.overlap_r[f];
    }

    if (fn >= static_cast<std::size_t>(k_overlap_len)) {
        std::ranges::fill(state.overlap_l, 0.0F);
        std::ranges::fill(state.overlap_r, 0.0F);
        return;
    }

    const auto remain = static_cast<std::size_t>(k_overlap_len) - fn;
    std::move(
        state.overlap_l.begin() + static_cast<std::ptrdiff_t>(fn), state.overlap_l.end(), state.overlap_l.begin());
    std::move(
        state.overlap_r.begin() + static_cast<std::ptrdiff_t>(fn), state.overlap_r.end(), state.overlap_r.begin());
    std::fill(state.overlap_l.begin() + static_cast<std::ptrdiff_t>(remain), state.overlap_l.end(), 0.0F);
    std::fill(state.overlap_r.begin() + static_cast<std::ptrdiff_t>(remain), state.overlap_r.end(), 0.0F);
}

// ── Source descriptor ─────────────────────────────────────────────────────────

// One renderable source extracted from the ADM scene.
struct BinauralSource {
    uint16_t channel_index{0};
    float gain{1.0F}; // object-level gain
    struct Block {
        float az{0.0F};
        float el{0.0F};
        float block_gain{1.0F};
        uint64_t start_sample{0};
        uint64_t end_sample{std::numeric_limits<uint64_t>::max()};
    };
    std::vector<Block> blocks;
};

// Resolve az/el from a SceneObjectBlock (handles Cartesian → polar).
std::pair<float, float> block_position(const SceneObject& obj, const SceneObjectBlock& blk) {
    const SceneBlockPosition pos =
        obj.position_offset ? apply_position_offset(blk.position, *obj.position_offset) : blk.position;
    if (pos.cartesian) {
        return cart_to_polar(pos.x, pos.y, pos.z);
    }
    return {pos.azimuth, pos.elevation};
}

// Build source list from scene.
std::vector<BinauralSource> build_sources(const AdmScene& scene, LogSink& logs) {
    std::vector<BinauralSource> srcs;

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
                BinauralSource src;
                src.channel_index = channel_index;
                src.gain = obj.gain;
                for (const auto& blk : track.blocks) {
                    auto [az, el] = block_position(obj, blk);
                    src.blocks.push_back(
                        {az, el, blk.gain, blk.start_sample, std::min(blk.end_sample, obj.end_sample)});
                }
                if (!src.blocks.empty()) {
                    std::ranges::sort(src.blocks, {}, &BinauralSource::Block::start_sample);
                    srcs.push_back(std::move(src));
                }
            }

            // DirectSpeakers-type blocks.
            for (const auto& ds : track.ds_blocks) {
                if (ds.low_pass_hz.has_value()) {
                    continue; // skip LFE
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
                src.blocks.push_back({az, el, ds.gain, ds.start_sample, std::min(ds.end_sample, obj.end_sample)});
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

    // One-time setup: build HRTF table + VBAP grid.
    progress.on_progress({RenderStage::planning, 0.1, "building HRTF table"});
    auto bs = build_binaural_state();
    if (!bs) {
        return make_error(ErrorCode::internal_error, "binaural: VBAP triangulation of HRTF directions failed", {});
    }

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
    saf_rfft_create(&hfft, k_fft_size);
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
    std::vector<OLAState> ola(sources.size());

    const uint64_t num_frames = info.num_frames;
    const uint16_t num_in_ch = info.num_channels;

    std::vector<float> in_block(static_cast<std::size_t>(num_in_ch) * k_block_size);
    std::vector<float> out_block(2U * k_block_size); // interleaved L/R
    uint64_t frames_done = 0;

    progress.on_progress({RenderStage::rendering, 0.2, "rendering"});

    while (frames_done < num_frames) {
        const uint64_t frames_now = std::min(k_block_size, num_frames - frames_done);
        const auto fn = static_cast<std::size_t>(frames_now);

        reader->read(in_block.data(), frames_now);
        std::ranges::fill(out_block, 0.0F);

        // Scratch L/R accumulation buffers (non-interleaved for SIMD friendliness).
        std::vector<float> l_buf(fn, 0.0F);
        std::vector<float> r_buf(fn, 0.0F);
        // De-interleaved input scratch for one channel.
        std::vector<float> ch_in(fn);

        for (std::size_t si = 0; si < sources.size(); ++si) {
            const auto& src = sources[si];
            if (src.blocks.empty()) {
                continue;
            }

            const uint64_t chunk_start = frames_done;
            const uint64_t chunk_end = frames_done + frames_now;
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

                const auto hrtf = hrtf_for_dir(*bs, blk.az, blk.el);
                const float gain = src.gain * blk.block_gain;
                convolve_and_accumulate(
                    hfft, ch_in.data(), seg_frames, gain, hrtf, ola[si], l_buf.data() + off, r_buf.data() + off);

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
    r.supported_layouts = {
        // clang-format off
        {"0+2+0", "Binaural (KEMAR HRTF, 836 dirs)", 2, false, 0, false, true},
        // clang-format on
    };
    return r;
}

std::unique_ptr<IRenderer> create_binaural_renderer() {
    return std::make_unique<BinauralRenderer>();
}

} // namespace mradm
