// avsamplebuffer_device.mm
//
// macOS-only IAudioOutputDevice backed by AVSampleBufferAudioRenderer. Instead of opening a
// raw multichannel hardware device (the miniaudio path), it enqueues the monitor's
// multichannel PCM (e.g. 7.1.4) into the system media playback stack, which spatializes it to
// the headphone route with dynamic head tracking. The monitor engine pulls exactly as it does
// for any IAudioOutputDevice; the AudioChannelLayoutTag (apple_layouts) is what makes the
// system treat the stream as spatial content. Apple framework types stay confined to adm_apple
// (ADR 0003); the factory returns the third-party-free IAudioOutputDevice interface.

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "adm/errors.h"

#include "apple_layouts.h"
#include "audio_output_device.h"

namespace mradm::realtime {
namespace {

// Frames per enqueued CMSampleBuffer (~21 ms @ 48 kHz). Small enough for responsive
// monitoring, large enough to keep enqueue overhead negligible.
constexpr std::size_t k_chunk_frames = 1024;
// Keep the system media queue moderately ahead while leaving room in MonitorEngine's local
// ring. This avoids the feed callback draining the local ring to 0 just because ASBR is ready
// for more media. ~128 ms at 48 kHz.
constexpr std::size_t k_target_queue_frames = 6144;
// Frames the system buffer must hold before the clock starts (prefill), so playback rides out
// worker jitter without starting from an empty queue. ~85 ms at 48 kHz.
constexpr std::size_t k_prefill_frames = 4096;
constexpr uint64_t k_feed_interval_ns = 10U * 1000U * 1000U;
constexpr uint64_t k_feed_leeway_ns = 2U * 1000U * 1000U;

class AVSampleBufferDevice final : public IAudioOutputDevice {
  public:
    explicit AVSampleBufferDevice(std::string layout_id) : layout_id_(std::move(layout_id)) {}
    ~AVSampleBufferDevice() override { stop(); }

    [[nodiscard]] Result<void> start(uint32_t channels, uint32_t sample_rate, PullFn pull) override {
        const auto* layout = apple_layouts::find_apple_speaker_layout(layout_id_);
        if (layout == nullptr) {
            return make_error(
                ErrorCode::unsupported, "AVSampleBufferAudioRenderer 不支持该监听布局", "layout=" + layout_id_);
        }
        if (channels != layout->channels) {
            return make_error(ErrorCode::invalid_argument,
                              "监听声道数与布局不符",
                              "layout=" + layout_id_ + " channels=" + std::to_string(channels));
        }
        channels_ = channels;
        sample_rate_ = sample_rate;
        pull_ = std::move(pull);
        pts_frames_ = 0;
        playing_started_ = false;
        staging_.assign(static_cast<std::size_t>(k_chunk_frames) * channels_, 0.0F);
        staged_frames_ = 0;

        if (!build_format(layout->layout_tag)) {
            return make_error(ErrorCode::render_failed, "创建 CMAudioFormatDescription 失败", "layout=" + layout_id_);
        }

        renderer_ = [[AVSampleBufferAudioRenderer alloc] init];
        // Declare multichannel content as spatializable so the system spatializes (and head-
        // tracks) it rather than just down-mixing. (Default already includes multichannel, but
        // setting it explicitly is the documented, future-proof contract.)
        renderer_.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMonoStereoAndMultichannel;
        synchronizer_ = [[AVSampleBufferRenderSynchronizer alloc] init];
        [synchronizer_ addRenderer:renderer_];
        queue_ = dispatch_queue_create("com.macinrender.asbr-monitor", DISPATCH_QUEUE_SERIAL);

        AVSampleBufferDevice* self = this; // raw; stop() drains the queue before teardown
        feed_timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        dispatch_source_set_timer(feed_timer_,
                                  dispatch_time(DISPATCH_TIME_NOW, 0),
                                  k_feed_interval_ns,
                                  k_feed_leeway_ns);
        dispatch_source_set_event_handler(feed_timer_, ^{
          self->feed_queue();
        });
        dispatch_resume(feed_timer_);

        [renderer_ requestMediaDataWhenReadyOnQueue:queue_
                                         usingBlock:^{
                                           self->feed_queue();
                                         }];
        return {};
    }

    void stop() override {
        if (renderer_ != nil) {
            [renderer_ stopRequestingMediaData];
        }
        dispatch_source_t timer = feed_timer_;
        feed_timer_ = nil;
        if (timer != nil) {
            dispatch_source_cancel(timer);
        }
        // Drain any in-flight enqueue block so the audio queue stops touching pull_/scratch_
        // before we release them (the block runs on queue_, a serial queue).
        if (queue_ != nil) {
            dispatch_sync(queue_, ^{
            });
        }
        if (synchronizer_ != nil) {
            [synchronizer_ setRate:0.0F time:kCMTimeZero];
        }
        if (renderer_ != nil) {
            [renderer_ flush];
        }
        synchronizer_ = nil;
        renderer_ = nil;
        queue_ = nil;
        if (format_ != nullptr) {
            CFRelease(format_);
            format_ = nullptr;
        }
        pull_ = nullptr;
    }

    [[nodiscard]] uint32_t actual_sample_rate() const override { return sample_rate_; }
    [[nodiscard]] bool pull_is_realtime_playback() const override { return false; }

    void flush() override {
        if (queue_ == nil) {
            return;
        }
        // Called on the worker (seek) thread. Hop to queue_ so staging_/pts_/the renderer are
        // touched only on the serial queue the feed block uses (no data race). The seek handshake
        // has already parked the ring pull, so the in-flight feed block here just no-ops.
        dispatch_sync(queue_, ^{
          staged_frames_ = 0; // drop the stale pre-seek partial so it can't splice onto new audio
          if (renderer_ != nil) {
              [renderer_ flush]; // drop ASBR's enqueued-but-unplayed buffers
          }
          // Re-prefill from the new position so PTS realigns to a fresh clock.
          playing_started_ = false;
          pts_frames_ = 0;
          if (synchronizer_ != nil) {
              [synchronizer_ setRate:0.0F time:kCMTimeZero];
          }
        });
    }

    void pause() override {
        if (queue_ == nil) {
            return;
        }
        dispatch_sync(queue_, ^{
          if (synchronizer_ != nil) {
              [synchronizer_ setRate:0.0F]; // freeze the clock (keep position) so it can't run away
          }
        });
    }

    void resume() override {
        if (queue_ == nil) {
            return;
        }
        dispatch_sync(queue_, ^{
          // Only resume the clock once prefill has actually started it; before that the prefill
          // gate in feed_queue owns setRate.
          if (playing_started_ && synchronizer_ != nil) {
              [synchronizer_ setRate:1.0F];
          }
        });
    }

  private:
    [[nodiscard]] bool build_format(AudioChannelLayoutTag tag) {
        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(sample_rate_);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mBytesPerPacket = static_cast<UInt32>(sizeof(float) * channels_);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = static_cast<UInt32>(sizeof(float) * channels_);
        asbd.mChannelsPerFrame = channels_;
        asbd.mBitsPerChannel = 32;

        AudioChannelLayout chlayout{};
        chlayout.mChannelLayoutTag = tag;
        const OSStatus st = CMAudioFormatDescriptionCreate(
            kCFAllocatorDefault, &asbd, sizeof(AudioChannelLayout), &chlayout, 0, nullptr, nullptr, &format_);
        return st == noErr && format_ != nullptr;
    }

    // Frames the synchronizer's clock has advanced (currentTime · sample_rate). 0 before playback
    // starts or before the clock is numeric — nothing has "played" yet in those cases.
    [[nodiscard]] int64_t clock_frames() const {
        if (!playing_started_ || synchronizer_ == nil || sample_rate_ == 0) {
            return 0;
        }
        const CMTime now = [synchronizer_ currentTime];
        if (!CMTIME_IS_NUMERIC(now)) {
            return 0;
        }
        const double seconds = CMTimeGetSeconds(now);
        if (!std::isfinite(seconds) || seconds <= 0.0) {
            return 0;
        }
        return static_cast<int64_t>(seconds * static_cast<double>(sample_rate_));
    }

    // Frames enqueued but not yet played (system queue depth). Drives feed throttling. Before
    // playback starts clock_frames() is 0, so this is the full enqueued count (the prefill gate).
    [[nodiscard]] std::size_t queued_frames() const {
        const int64_t q = pts_frames_ - clock_frames();
        return q > 0 ? static_cast<std::size_t>(q) : 0;
    }

    void feed_queue() {
        if (renderer_ == nil) {
            return;
        }

        while ([renderer_ isReadyForMoreMediaData] && queued_frames() < k_target_queue_frames) {
            CMSampleBufferRef sb = next_buffer();
            if (sb == nullptr) {
                break; // ring didn't yield a full chunk: leave the partial staged, refill next tick
            }
            [renderer_ enqueueSampleBuffer:sb];
            CFRelease(sb);
        }

        if (!playing_started_ && queued_frames() >= k_prefill_frames) {
            [synchronizer_ setRate:1.0F time:kCMTimeZero];
            playing_started_ = true;
        }
    }

    // Fill the staging block to exactly k_chunk_frames from the ring, then wrap it as a ready,
    // fixed-size CMSampleBuffer with contiguous PTS. Returns nullptr — leaving any partial staged
    // for the next call — when the ring can't complete a full block, so ASBR never sees a variable-
    // size buffer or a silence-padded tail. +1-retained; caller CFReleases. Runs on queue_, the
    // only thread touching staging_/staged_frames_/pts_.
    [[nodiscard]] CMSampleBufferRef next_buffer() {
        while (staged_frames_ < k_chunk_frames) {
            const std::size_t need = k_chunk_frames - staged_frames_;
            const std::size_t produced =
                pull_ ? pull_(std::span<float>(staging_.data() + (staged_frames_ * channels_), need * channels_), need)
                      : 0;
            staged_frames_ += produced;
            if (produced < need) {
                break; // ring drained mid-fill — keep what we have, finish the block next time
            }
        }
        if (staged_frames_ < k_chunk_frames) {
            return nullptr; // not a full block yet
        }

        const std::size_t byte_count = static_cast<std::size_t>(k_chunk_frames) * channels_ * sizeof(float);
        CMBlockBufferRef block = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, byte_count, kCFAllocatorDefault, nullptr, 0, byte_count, 0, &block);
        if (st != kCMBlockBufferNoErr || block == nullptr) {
            return nullptr;
        }
        st = CMBlockBufferReplaceDataBytes(staging_.data(), block, 0, byte_count);
        if (st != kCMBlockBufferNoErr) {
            CFRelease(block);
            return nullptr;
        }

        // Rebase PTS after a real underflow: if the clock has advanced past everything enqueued,
        // start this buffer at the clock — not at the stale pts_frames_, which would land in the
        // clock's past and be dropped / garbled by ASBR.
        const int64_t clock = clock_frames();
        if (clock > pts_frames_) {
            pts_frames_ = clock;
        }
        CMSampleBufferRef sample = nullptr;
        const CMTime pts = CMTimeMake(pts_frames_, static_cast<int32_t>(sample_rate_));
        st = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, block, format_, static_cast<CMItemCount>(k_chunk_frames), pts, nullptr, &sample);
        CFRelease(block);
        if (st != noErr || sample == nullptr) {
            return nullptr;
        }
        pts_frames_ += static_cast<int64_t>(k_chunk_frames);
        staged_frames_ = 0;
        return sample;
    }

    std::string layout_id_;
    uint32_t channels_{0};
    uint32_t sample_rate_{0};
    PullFn pull_;
    int64_t pts_frames_{0};
    // Accumulate into fixed-size k_chunk_frames blocks before enqueuing. Feeding ASBR variable-
    // size buffers (the raw per-pull `produced`) caused intermittent artifacts / blowups over a
    // long stream; every enqueued buffer is now exactly k_chunk_frames with contiguous PTS.
    std::vector<float> staging_;
    std::size_t staged_frames_{0};
    bool playing_started_{false}; // prefill gate: setRate fires once queued_frames() ≥ k_prefill_frames

    AVSampleBufferAudioRenderer* renderer_{nil};
    AVSampleBufferRenderSynchronizer* synchronizer_{nil};
    dispatch_queue_t queue_{nil};
    dispatch_source_t feed_timer_{nil};
    CMAudioFormatDescriptionRef format_{nullptr};
};

} // namespace

std::unique_ptr<IAudioOutputDevice> make_avsamplebuffer_device(std::string layout_id) {
    return std::make_unique<AVSampleBufferDevice>(std::move(layout_id));
}

} // namespace mradm::realtime
