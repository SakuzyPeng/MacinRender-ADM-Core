#include "binaural_spreader.h"

#include <algorithm>
#include <cstring>
#include <utility>

extern "C" {
#include "spreader_mr.h"
}

namespace mradm {

BinauralSpreaderAdapter::BinauralSpreaderAdapter(
    const float* hrtf_td, const float* grid_dirs_deg, int num_dirs, int hrir_len, int sample_rate, int n_sources)
    : n_sources_(std::min(n_sources, k_max_sources)) {
    spreader_create(&handle_);
    spreader_init(handle_, sample_rate);
    spreader_setNumSources(handle_, n_sources_);
    spreader_setSpreadingMode(handle_, SPREADER_MODE_OM);
    spreader_setAveragingCoeff(handle_, 0.9F);

    spreader_init_from_hrtf_grid(handle_, hrtf_td, grid_dirs_deg, num_dirs, k_n_ears, hrir_len, sample_rate);
    spreader_initCodec(handle_);

    carry_.assign(static_cast<std::size_t>(n_sources_),
                  std::vector<float>(static_cast<std::size_t>(k_frame_size), 0.0F));
    in_scratch_.assign(static_cast<std::size_t>(n_sources_),
                       std::vector<float>(static_cast<std::size_t>(k_frame_size), 0.0F));
    out_scratch_l_.assign(static_cast<std::size_t>(k_frame_size), 0.0F);
    out_scratch_r_.assign(static_cast<std::size_t>(k_frame_size), 0.0F);

    in_ptrs_.resize(static_cast<std::size_t>(n_sources_));
    for (int i = 0; i < n_sources_; ++i) {
        in_ptrs_[static_cast<std::size_t>(i)] = in_scratch_[static_cast<std::size_t>(i)].data();
    }
    out_ptrs_ = {out_scratch_l_.data(), out_scratch_r_.data()};

    out_ring_l_.assign(k_ring_cap, 0.0F);
    out_ring_r_.assign(k_ring_cap, 0.0F);
}

BinauralSpreaderAdapter::~BinauralSpreaderAdapter() {
    if (handle_ != nullptr) {
        spreader_destroy(&handle_);
    }
}

BinauralSpreaderAdapter::BinauralSpreaderAdapter(BinauralSpreaderAdapter&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), n_sources_(other.n_sources_), gains_(other.gains_),
      carry_(std::move(other.carry_)), carry_fill_(other.carry_fill_), in_scratch_(std::move(other.in_scratch_)),
      out_scratch_l_(std::move(other.out_scratch_l_)), out_scratch_r_(std::move(other.out_scratch_r_)),
      in_ptrs_(std::move(other.in_ptrs_)), out_ptrs_(std::move(other.out_ptrs_)),
      out_ring_l_(std::move(other.out_ring_l_)), out_ring_r_(std::move(other.out_ring_r_)),
      out_ring_write_(other.out_ring_write_), out_ring_fill_(other.out_ring_fill_) {
    for (int i = 0; i < n_sources_; ++i) {
        in_ptrs_[static_cast<std::size_t>(i)] = in_scratch_[static_cast<std::size_t>(i)].data();
    }
    out_ptrs_ = {out_scratch_l_.data(), out_scratch_r_.data()};
}

BinauralSpreaderAdapter& BinauralSpreaderAdapter::operator=(BinauralSpreaderAdapter&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            spreader_destroy(&handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
        n_sources_ = other.n_sources_;
        gains_ = other.gains_;
        carry_ = std::move(other.carry_);
        carry_fill_ = other.carry_fill_;
        in_scratch_ = std::move(other.in_scratch_);
        out_scratch_l_ = std::move(other.out_scratch_l_);
        out_scratch_r_ = std::move(other.out_scratch_r_);
        in_ptrs_ = std::move(other.in_ptrs_);
        out_ptrs_ = std::move(other.out_ptrs_);
        out_ring_l_ = std::move(other.out_ring_l_);
        out_ring_r_ = std::move(other.out_ring_r_);
        out_ring_write_ = other.out_ring_write_;
        out_ring_fill_ = other.out_ring_fill_;
        for (int i = 0; i < n_sources_; ++i) {
            in_ptrs_[static_cast<std::size_t>(i)] = in_scratch_[static_cast<std::size_t>(i)].data();
        }
        out_ptrs_ = {out_scratch_l_.data(), out_scratch_r_.data()};
    }
    return *this;
}

void BinauralSpreaderAdapter::set_source(int idx, float az_deg, float el_deg, float spread_deg, float gain) {
    if (idx < 0 || idx >= n_sources_) {
        return;
    }
    spreader_setSourceAzi_deg(handle_, idx, az_deg);
    spreader_setSourceElev_deg(handle_, idx, el_deg);
    spreader_setSourceSpread_deg(handle_, idx, spread_deg);
    gains_.at(static_cast<std::size_t>(idx)) = gain;
}

// Apply gain, call spreader_process, push 512 output samples into the ring.
void BinauralSpreaderAdapter::push_batch() {
    // Apply per-source gain.
    for (int si = 0; si < n_sources_; ++si) {
        const float g = gains_.at(static_cast<std::size_t>(si));
        auto& buf = in_scratch_[static_cast<std::size_t>(si)];
        std::ranges::transform(buf, buf.begin(), [g](float s) { return s * g; });
    }

    std::ranges::fill(out_scratch_l_, 0.0F);
    std::ranges::fill(out_scratch_r_, 0.0F);

    spreader_process(handle_, in_ptrs_.data(), out_ptrs_.data(), n_sources_, k_n_ears, k_frame_size);

    // Write 512 samples into the ring (wrapping).
    for (int f = 0; f < k_frame_size; ++f) {
        const std::size_t idx = (out_ring_write_ + static_cast<std::size_t>(f)) % k_ring_cap;
        out_ring_l_[idx] = out_scratch_l_[static_cast<std::size_t>(f)];
        out_ring_r_[idx] = out_scratch_r_[static_cast<std::size_t>(f)];
    }
    out_ring_write_ = (out_ring_write_ + static_cast<std::size_t>(k_frame_size)) % k_ring_cap;
    out_ring_fill_ += static_cast<std::size_t>(k_frame_size);
}

// Drain n_frames samples from the ring, accumulating into l_out / r_out.
void BinauralSpreaderAdapter::drain_ring(float* l_out, float* r_out, std::size_t n_frames) {
    const std::size_t to_read = std::min(n_frames, out_ring_fill_);
    const std::size_t read_start = (out_ring_write_ + k_ring_cap - out_ring_fill_) % k_ring_cap;

    for (std::size_t f = 0; f < to_read; ++f) {
        const std::size_t idx = (read_start + f) % k_ring_cap;
        l_out[f] += out_ring_l_[idx];
        r_out[f] += out_ring_r_[idx];
    }
    out_ring_fill_ -= to_read;
}

void BinauralSpreaderAdapter::process_chunk(
    const float* const* mono_ins, int n_sources_in, std::size_t n_frames, float* l_out, float* r_out) {
    const int n_in = std::min(n_sources_in, n_sources_);
    std::size_t src_pos = 0;

    // Feed all n_frames into carry buffer, draining complete 512-sample batches.
    while (src_pos < n_frames) {
        const std::size_t space = static_cast<std::size_t>(k_frame_size) - carry_fill_;
        const std::size_t avail = n_frames - src_pos;
        const std::size_t to_copy = std::min(space, avail);

        for (int si = 0; si < n_in; ++si) {
            std::copy_n(mono_ins[si] + src_pos,
                        to_copy,
                        carry_[static_cast<std::size_t>(si)].begin() + static_cast<std::ptrdiff_t>(carry_fill_));
        }
        for (int si = n_in; si < n_sources_; ++si) {
            std::fill_n(
                carry_[static_cast<std::size_t>(si)].begin() + static_cast<std::ptrdiff_t>(carry_fill_), to_copy, 0.0F);
        }
        carry_fill_ += to_copy;
        src_pos += to_copy;

        if (carry_fill_ < static_cast<std::size_t>(k_frame_size)) {
            break;
        }

        // Copy carry → in_scratch, process, push output to ring.
        for (int si = 0; si < n_sources_; ++si) {
            std::copy(carry_[static_cast<std::size_t>(si)].begin(),
                      carry_[static_cast<std::size_t>(si)].end(),
                      in_scratch_[static_cast<std::size_t>(si)].begin());
        }
        carry_fill_ = 0;
        push_batch();
    }

    // Drain n_frames samples from the ring into l_out / r_out.
    drain_ring(l_out, r_out, n_frames);
}

int BinauralSpreaderAdapter::processing_delay() {
    return spreader_getProcessingDelay();
}

int BinauralSpreaderAdapter::max_sources() {
    return k_max_sources;
}

int BinauralSpreaderAdapter::total_latency() {
    // STFT processing delay plus the one-frame prime cushion (see prime()).
    return processing_delay() + k_frame_size;
}

void BinauralSpreaderAdapter::prime() {
    // Pre-fill the output ring with exactly one frame (k_frame_size) of warm-up by
    // pushing a single zero batch *without* draining. This establishes the invariant
    // out_ring_fill_ + carry_fill_ == k_frame_size, which holds across every later
    // process_chunk() (push n, drain n keeps the sum constant). One frame of slack is
    // the minimum that guarantees the ring never underflows for an arbitrary, non-512-
    // aligned n_frames, so each process_chunk() drains *exactly* n_frames with no carry
    // jitter and no output gaps. The adapter's resulting input→output latency is the
    // constant total_latency() = processing_delay() + k_frame_size; the caller fully
    // compensates for it (OLA-sync delay + head-skip + silent tail of total_latency()).
    // NB: priming with more frames would only push real output further back — the STFT's
    // own processing_delay() is already absorbed because spreader_process is causal.
    for (auto& buf : in_scratch_) {
        std::ranges::fill(buf, 0.0F);
    }
    push_batch();
}

} // namespace mradm
