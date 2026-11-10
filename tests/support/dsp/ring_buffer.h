#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

// Single-producer / single-consumer lock-free float ring buffer. The realtime monitor's
// worker thread is the sole producer (pushes rendered PCM ahead of the playhead); the
// audio device callback is the sole consumer (pops what it needs). Stores raw floats
// (interleaved PCM is the caller's framing concern). Wait-free on both sides: each side
// owns one index and only ever reads the other's index. See
// docs/architecture/REALTIME_MONITORING.md §4 / REALTIME_MONITORING_SLICE1.md §3.
namespace mradm::realtime {

class FloatRingBuffer {
  public:
    // `capacity` is the number of floats that can be held at once. One extra slot is
    // reserved internally so a full buffer is distinguishable from an empty one.
    explicit FloatRingBuffer(std::size_t capacity) : buf_(capacity + 1U) {}

    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size() - 1U; }

    // Consumer-side query: floats available to pop now (acquire on write_ to observe the
    // producer's latest commit). available_read()/available_write() each load the *other*
    // side's index with acquire so they are accurate when called from their own side; a
    // cross-side call is only an approximate status metric, never a synchronisation
    // primitive — push()/pop() return the truly transferred count.
    [[nodiscard]] std::size_t available_read() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return (w + buf_.size() - r) % buf_.size();
    }

    // Producer-side query: floats that can be pushed now (acquire on read_ to observe the
    // consumer's latest drain). Mirrors push()'s own free-space computation.
    [[nodiscard]] std::size_t available_write() const noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return (r + buf_.size() - w - 1U) % buf_.size();
    }

    // Producer side. Writes up to `n` floats from `src`; returns how many were written
    // (fewer than `n` when the buffer fills).
    std::size_t push(const float* src, std::size_t n) noexcept {
        std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t free_floats = (r + buf_.size() - w - 1U) % buf_.size();
        const std::size_t count = std::min(n, free_floats);
        for (std::size_t i = 0; i < count; ++i) {
            buf_[w] = src[i];
            w = (w + 1U) % buf_.size();
        }
        write_.store(w, std::memory_order_release);
        return count;
    }

    // Reset to empty. NOT thread-safe: only call when neither push() nor pop() can run
    // concurrently (the monitor's seek handshake guarantees the consumer is idle first).
    void clear() noexcept {
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    // Consumer side. Reads up to `n` floats into `dst`; returns how many were read
    // (fewer than `n` when the buffer drains — an underrun the caller pads with silence).
    std::size_t pop(float* dst, std::size_t n) noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t avail = (w + buf_.size() - r) % buf_.size();
        const std::size_t count = std::min(n, avail);
        for (std::size_t i = 0; i < count; ++i) {
            dst[i] = buf_[r];
            r = (r + 1U) % buf_.size();
        }
        read_.store(r, std::memory_order_release);
        return count;
    }

  private:
    std::vector<float> buf_;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

} // namespace mradm::realtime
