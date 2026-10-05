#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <list>
#include <utility>

#include "adm/audio_io.h"

#include "render_common.h"
#include "render_state.h"

namespace mradm::triple_balance {
namespace {
constexpr uint64_t k_block_frames = 1024;
constexpr std::size_t k_checkpoint_bytes = std::size_t{32} * 1024U * 1024U;

class TripleBalanceStream final : public IRenderStream {
  public:
    static Result<std::unique_ptr<IRenderStream>>
    create(const Prepared& prepared, const RenderPlan& plan, std::size_t checkpoint_budget) {
        if (plan.scene.info.num_channels == 0 || prepared.output_channels == 0 ||
            prepared.sample_rate != plan.scene.info.sample_rate) {
            return make_error(ErrorCode::invalid_argument, "triple-balance: invalid stream format");
        }
        auto reader = audio::RenderInputReader::open(plan.input_path, false);
        if (!reader) {
            return tl::unexpected{reader.error()};
        }
        auto mixer = SizeMixer::create(prepared, plan, true);
        if (!mixer) {
            return tl::unexpected{mixer.error()};
        }
        return std::unique_ptr<IRenderStream>{
            new TripleBalanceStream(prepared, plan, std::move(*reader), std::move(*mixer), checkpoint_budget)};
    }

    Result<std::size_t> process(std::span<float> out, std::size_t frames) override {
        if (frames > out.size() / out_channels()) {
            return make_error(ErrorCode::invalid_argument, "triple-balance: output buffer is too small");
        }
        try {
            std::size_t produced = 0;
            while (produced < frames) {
                if (fifo_read_ == fifo_.size()) {
                    if (position_ == total_) {
                        break;
                    }
                    auto status = render_block();
                    if (!status) {
                        return tl::unexpected{status.error()};
                    }
                }
                const auto take = std::min(frames - produced, (fifo_.size() - fifo_read_) / out_channels());
                std::copy_n(fifo_.data() + fifo_read_, take * out_channels(), out.data() + (produced * out_channels()));
                fifo_read_ += take * out_channels();
                produced += take;
            }
            return produced;
        } catch (const std::exception& error) {
            return make_error(ErrorCode::io_error, error.what(), "triple-balance stream");
        }
    }

    [[nodiscard]] Result<void> validate_overrides(const LiveOverrides& overrides) const override {
        for (const auto& item : overrides.objects) {
            const float width = item.extent_scale * item.extent_width_scale;
            const float height = item.extent_scale * item.extent_height_scale;
            const float depth = item.extent_scale * item.extent_depth_scale;
            if (!std::isfinite(item.gain_db) || !std::isfinite(std::pow(10.0F, item.gain_db / 20.0F)) ||
                !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(depth) || item.extent_scale < 0 ||
                item.extent_width_scale < 0 || item.extent_height_scale < 0 || item.extent_depth_scale < 0) {
                return make_error(ErrorCode::invalid_argument,
                                  "Triple Balance requires finite nonnegative size scales",
                                  item.object_id);
            }
            if (width != height || width != depth || item.diffuse_scale != 1 || item.divergence_scale != 1 ||
                item.head_locked.value_or(false)) {
                return make_error(ErrorCode::unsupported,
                                  "Triple Balance live editing supports gain, mute and linked isotropic size only",
                                  item.object_id);
            }
            const bool bed = std::ranges::any_of(prepared_.gain_matrix, [&](const auto& channel) {
                return channel.object_id == item.object_id && !channel.speaker_label_key.empty();
            });
            if (width != 1 && (bed || !item.speaker_label.empty())) {
                return make_error(
                    ErrorCode::unsupported, "Triple Balance size editing applies to Objects only", item.object_id);
            }
            if (prepared_.spread_mode == SpeakerSpreadMode::none && width != 1) {
                return make_error(
                    ErrorCode::unsupported, "Triple Balance size editing requires spread=auto", item.object_id);
            }
            const bool pure_diffuse_point = std::ranges::any_of(prepared_.size_tracks, [&](const auto& track) {
                return track.object_id == item.object_id && track.has_diffuse &&
                       track.minimum_diffuse_size * width == 0;
            });
            if (pure_diffuse_point) {
                return make_error(ErrorCode::unsupported,
                                  "Triple Balance does not support a pure diffuse point source",
                                  item.object_id);
            }
        }
        return {};
    }

    void set_overrides(const LiveOverrides& overrides) override {
        if (!validate_overrides(overrides)) {
            return;
        }
        std::ranges::fill(gain_targets_, 1.0F);
        for (const auto& channel : prepared_.gain_matrix) {
            if (auto gain =
                    render_common::resolve_live_channel_gain(overrides, channel.object_id, channel.speaker_label_key)) {
                gain_targets_[channel.input_channel] = *gain;
            }
        }
        ramps_.set_targets(gain_targets_);
        auto scales = scales_;
        std::ranges::fill(scales, 1.0F);
        for (std::size_t i = 0; i < prepared_.size_tracks.size(); ++i) {
            for (const auto& item : overrides.objects) {
                if (item.object_id == prepared_.size_tracks[i].object_id && item.speaker_label.empty()) {
                    scales[i] = item.extent_scale * item.extent_width_scale;
                    // Nonnegative multiplication is monotonic, including underflow: this summary
                    // preserves the old all-events-zero test without retaining the event table.
                    if (prepared_.size_tracks[i].maximum_source_size * scales[i] == 0) {
                        scales[i] = 0;
                    }
                }
            }
        }
        if (scales != scales_) {
            const bool initial = position_ == 0 && fifo_.empty();
            scales_ = std::move(scales);
            checkpoints_.clear();
            canonical_ = initial;
            if (initial) {
                configure_matrix();
            } else {
                // A formerly all-point track joins the size mix at the next render block.
                // Keep the verified point timeline for the one-control-block transition.
                std::vector<float> active(scales_.size(), 1.0F);
                mixer_.set_point_in_matrix(active);
            }
            mixer_.set_scales(scales_, initial);
        }
    }

    Result<void> seek(uint64_t frame) override { return seek_with_cancel(frame, {}); }
    [[nodiscard]] bool seek_requires_preroll() const override { return !prepared_.size_tracks.empty(); }

    Result<void> seek_with_cancel(uint64_t frame, std::stop_token cancel) override {
        if (cancel.stop_requested()) {
            return make_error(ErrorCode::cancelled, "Triple Balance seek cancelled");
        }
        try {
            const auto target = std::min(frame, total_);
            const auto boundary = target / k_block_frames * k_block_frames;
            configure_matrix();
            mixer_.reset();
            mixer_.set_scales(scales_, true);
            gain_mix_.reset();
            position_ = prepared_.size_tracks.empty() ? boundary : 0;
            auto best = checkpoints_.end();
            for (auto it = checkpoints_.begin(); it != checkpoints_.end(); ++it) {
                if (it->frame <= boundary && (best == checkpoints_.end() || it->frame > best->frame)) {
                    best = it;
                }
            }
            if (best != checkpoints_.end()) {
                position_ = best->frame;
                mixer_.restore(best->states);
                checkpoints_.splice(checkpoints_.begin(), checkpoints_, best);
            }
            fifo_.clear();
            fifo_read_ = 0;
            canonical_ = true;
            reset_ramps();
            render_common::seek_reader_abs(*reader_, position_);
            while (position_ < boundary) {
                if (cancel.stop_requested()) {
                    return make_error(ErrorCode::cancelled, "Triple Balance seek cancelled");
                }
                auto status = render_block();
                if (!status) {
                    return status;
                }
            }
            // Pin the current loop/seek anchor before rendering the containing block.
            anchor_ = boundary;
            save_checkpoint(true);
            fifo_.clear();
            fifo_read_ = 0;
            if (target > position_) {
                auto status = render_block();
                if (!status) {
                    return status;
                }
                fifo_read_ = static_cast<std::size_t>(target - boundary) * out_channels();
            }
            return {};
        } catch (const std::exception& error) {
            return make_error(ErrorCode::io_error, error.what(), "triple-balance seek");
        }
    }

    [[nodiscard]] uint32_t out_channels() const override { return prepared_.output_channels; }
    [[nodiscard]] uint32_t sample_rate() const override { return prepared_.sample_rate; }
    [[nodiscard]] std::string_view output_layout() const override { return layout_; }

  private:
    struct Checkpoint {
        uint64_t frame{};
        dsp::TbSnapshot states{nullptr, mradm_dsp_tb_snapshot_destroy};
    };

    TripleBalanceStream(const Prepared& prepared,
                        const RenderPlan& plan,
                        std::unique_ptr<audio::RenderInputReader> reader,
                        SizeMixer mixer,
                        std::size_t checkpoint_budget)
        : prepared_(prepared), reader_(std::move(reader)), mixer_(std::move(mixer)),
          input_channels_(plan.scene.info.num_channels), total_(plan.scene.info.num_frames),
          layout_(plan.output_layout), default_interp_(uint64_t{sample_rate()} * plan.default_interp_ms / 1000),
          checkpoint_interval_(((uint64_t{sample_rate()} + k_block_frames - 1) / k_block_frames) * k_block_frames),
          input_(k_block_frames * input_channels_), envelopes_(input_.size()),
          gain_mix_(prepared.gain_matrix.plan, k_block_frames, default_interp_, false),
          gain_targets_(input_channels_, 1.0F), ramps_(input_channels_, sample_rate()),
          scales_(prepared.size_tracks.size(), 1.0F),
          checkpoint_budget_(std::min(checkpoint_budget, k_checkpoint_bytes)),
          size_by_channel_(input_channels_, prepared.size_tracks.size()) {
        for (std::size_t i = 0; i < prepared.size_tracks.size(); ++i) {
            size_by_channel_[prepared.size_tracks[i].input_channel] = i;
        }
        reset_ramps();
        fifo_.reserve(k_block_frames * out_channels());
    }

    void configure_matrix() { mixer_.set_point_in_matrix(scales_); }

    void reset_ramps() {
        ramps_.reset();
        ramps_.set_targets(gain_targets_);
    }

    void save_checkpoint(bool force = false) {
        if (!canonical_ || prepared_.size_tracks.empty() || position_ == 0 || position_ % k_block_frames != 0 ||
            (!force && position_ % checkpoint_interval_ != 0)) {
            return;
        }
        const auto bytes = sizeof(Checkpoint) + (2 * sizeof(void*)) + mixer_.snapshot_bytes();
        const auto capacity = checkpoint_budget_ / bytes;
        if (capacity == 0) {
            return;
        }
        if (auto existing = std::ranges::find(checkpoints_, position_, &Checkpoint::frame);
            existing != checkpoints_.end()) {
            checkpoints_.splice(checkpoints_.begin(), checkpoints_, existing);
            return;
        }
        while (checkpoints_.size() >= capacity) {
            auto victim = std::prev(checkpoints_.end());
            if (victim->frame == anchor_) {
                if (checkpoints_.size() == 1 && !force) {
                    return;
                }
                if (checkpoints_.size() > 1) {
                    --victim;
                }
            }
            checkpoints_.erase(victim);
        }
        checkpoints_.push_front({position_, mixer_.snapshot()});
    }

    Result<void> render_block() {
        const auto frames = std::min(k_block_frames, total_ - position_);
        if (reader_->read(input_.data(), frames) != frames) {
            return make_error(ErrorCode::io_error, "short input read in Triple Balance stream");
        }
        ramps_.fill(std::span{envelopes_}.first(frames * input_channels_));
        fifo_.assign(frames * out_channels(), 0.0F);
        fifo_read_ = 0;
        const render_common::AccumulateContext context{input_.data(),
                                                       &fifo_,
                                                       position_,
                                                       input_channels_,
                                                       prepared_.output_channels,
                                                       default_interp_,
                                                       0,
                                                       envelopes_};
        auto points = mixer_.prepare_points(position_, frames);
        if (!points) {
            return points;
        }
        for (std::size_t i = 0; i < prepared_.gain_matrix.size(); ++i) {
            const auto& channel = prepared_.gain_matrix[i];
            const auto size_track = size_by_channel_[channel.input_channel];
            if (canonical_ && size_track < scales_.size() && scales_[size_track] == 0) {
                mixer_.accumulate_point(size_track, context, frames, true);
            } else {
                gain_mix_.speaker(
                    std::span{input_}.first(frames * input_channels_), fifo_, envelopes_, position_, frames, i);
            }
        }
        auto status = mixer_.process(std::span<const float>(input_.data(), frames * input_channels_),
                                     fifo_,
                                     position_,
                                     position_ + frames == total_,
                                     envelopes_);
        if (!status) {
            return status;
        }
        position_ += frames;
        save_checkpoint();
        return {};
    }

    const Prepared& prepared_;
    std::unique_ptr<audio::RenderInputReader> reader_;
    SizeMixer mixer_;
    uint16_t input_channels_;
    uint64_t total_;
    std::string layout_;
    uint64_t default_interp_{};
    uint64_t checkpoint_interval_{};
    std::vector<float> input_;
    std::vector<float> envelopes_;
    dsp::PcmMixer gain_mix_;
    std::vector<float> gain_targets_;
    render_common::InterleavedLiveGainSmoother ramps_;
    std::vector<float> scales_;
    std::vector<float> fifo_;
    std::size_t fifo_read_{};
    uint64_t position_{};
    uint64_t anchor_{};
    bool canonical_{true};
    std::size_t checkpoint_budget_;
    std::vector<std::size_t> size_by_channel_;
    std::list<Checkpoint> checkpoints_;
};
} // namespace

Result<std::unique_ptr<IRenderStream>>
open_stream(const Prepared& prepared, const RenderPlan& plan, std::size_t checkpoint_budget) {
    return TripleBalanceStream::create(prepared, plan, checkpoint_budget);
}
} // namespace mradm::triple_balance
