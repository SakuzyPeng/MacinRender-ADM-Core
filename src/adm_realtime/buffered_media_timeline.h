#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

namespace mradm::realtime {

// Queue-thread-only PTS -> media mapping. Transport gaps and padding never advance
// the media cursor. Retire played entries, so storage is bounded by device lead.
class BufferedMediaTimeline {
  public:
    void reset() {
        segments_.clear();
        enqueued_ = 0;
        presented_ = 0;
    }

    void enqueue(std::int64_t pts, std::uint32_t valid_frames) {
        if (valid_frames != 0U) {
            segments_.push_back({pts, enqueued_, valid_frames});
            enqueued_ += valid_frames;
        }
    }

    [[nodiscard]] std::uint64_t advance(std::int64_t pts) {
        while (!segments_.empty()) {
            const auto& segment = segments_.front();
            if (pts <= segment.pts) {
                break;
            }
            const auto played = std::min<std::uint64_t>(static_cast<std::uint64_t>(pts - segment.pts), segment.frames);
            presented_ = std::max(presented_, segment.media_start + played);
            if (played < segment.frames) {
                break;
            }
            segments_.pop_front();
        }
        return presented_;
    }

    [[nodiscard]] std::uint64_t enqueued() const { return enqueued_; }

  private:
    struct Segment {
        std::int64_t pts;
        std::uint64_t media_start;
        std::uint32_t frames;
    };
    std::deque<Segment> segments_;
    std::uint64_t enqueued_{0};
    std::uint64_t presented_{0};
};

} // namespace mradm::realtime
