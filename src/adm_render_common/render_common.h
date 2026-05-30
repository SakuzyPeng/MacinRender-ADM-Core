#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <string_view>
#include <thread>
#include <vector>

#include "adm/logging.h"
#include "adm/scene.h"

namespace mradm::render_common {

// Single background thread that runs posted tasks strictly in FIFO order. Renderers use it to move
// the loudness / true-peak measurement (libebur128) off the critical path so it overlaps with the
// next block's read + mix. Because tasks run in submission order on one thread, the sequence of
// ebur128 calls — and therefore the measured loudness / true peak — is bit-identical to running them
// inline; only the audio output buffer must be double-buffered so the next block does not overwrite
// a buffer still being measured (wait on the returned future before reuse).
class SerialWorker {
  public:
    SerialWorker();
    ~SerialWorker();
    SerialWorker(const SerialWorker&) = delete;
    SerialWorker& operator=(const SerialWorker&) = delete;
    SerialWorker(SerialWorker&&) = delete;
    SerialWorker& operator=(SerialWorker&&) = delete;

    // Enqueue a task; the returned future becomes ready once the task has run.
    std::future<void> post(std::function<void()> task);

  private:
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::packaged_task<void()>> tasks_;
    bool stop_{false};
    std::thread thread_;
};

struct PreparedObjectBlock {
    std::vector<SceneObjectBlock> sources;
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    bool jump_position{false};
    std::optional<uint64_t> interp_length_samples;
};

[[nodiscard]] PreparedObjectBlock prepare_object_block(const SceneObjectBlock& raw_block,
                                                       const SceneObject& object,
                                                       const std::vector<SceneOutputSpeaker>& speakers,
                                                       LogSink& logs,
                                                       std::string_view log_module,
                                                       bool& screen_ref_warned);

template <typename Channel>
[[nodiscard]] uint64_t block_active_length(const Channel& channel, std::size_t block_index) {
    const auto& block = channel.blocks[block_index];
    uint64_t active_end = block.end_sample;
    if (block_index + 1 < channel.blocks.size()) {
        active_end = std::min(active_end, channel.blocks[block_index + 1].start_sample);
    }
    if (active_end <= block.start_sample) {
        return 0;
    }
    return active_end - block.start_sample;
}

template <typename Channel>
[[nodiscard]] uint64_t interpolation_length(const Channel& channel, std::size_t block_index, uint64_t default_interp) {
    const auto& block = channel.blocks[block_index];
    if (block.jump_position || block_index == 0) {
        return 0;
    }
    return std::min(block.interp_length_samples.value_or(default_interp), block_active_length(channel, block_index));
}

template <typename T> [[nodiscard]] T interpolated_scalar(T previous, T current, uint64_t delta, uint64_t interp_len) {
    const auto alpha = static_cast<double>(delta) / static_cast<double>(interp_len);
    return static_cast<T>((static_cast<double>(previous) * (1.0 - alpha)) + (static_cast<double>(current) * alpha));
}

} // namespace mradm::render_common
