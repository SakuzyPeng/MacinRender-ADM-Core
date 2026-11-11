#include "room_worker_wait.h"

#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// avrt.h requires the Windows handle types first.
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#endif

namespace mradm::realtime {

#ifdef _WIN32
namespace {
class RoomWaitTimer {
  public:
    RoomWaitTimer()
        : timer_(CreateWaitableTimerExW(
              nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
    ~RoomWaitTimer() {
        if (timer_ != nullptr) {
            CloseHandle(timer_);
        }
    }
    RoomWaitTimer(const RoomWaitTimer&) = delete;
    RoomWaitTimer& operator=(const RoomWaitTimer&) = delete;
    RoomWaitTimer(RoomWaitTimer&&) = delete;
    RoomWaitTimer& operator=(RoomWaitTimer&&) = delete;
    [[nodiscard]] bool wait(std::chrono::milliseconds duration) const {
        if (timer_ == nullptr) {
            return false;
        }
        LARGE_INTEGER due{};
        due.QuadPart = -duration.count() * 10000;
        return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != 0 &&
               WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0;
    }

  private:
    HANDLE timer_;
};
class RoomAudioTask {
  public:
    RoomAudioTask() = default;
    ~RoomAudioTask() { set_active(false); }
    RoomAudioTask(const RoomAudioTask&) = delete;
    RoomAudioTask& operator=(const RoomAudioTask&) = delete;
    RoomAudioTask(RoomAudioTask&&) = delete;
    RoomAudioTask& operator=(RoomAudioTask&&) = delete;
    void set_active(bool active) {
        if (active == active_) {
            return;
        }
        active_ = active;
        if (active) {
            DWORD index = 0;
            task_ = AvSetMmThreadCharacteristicsW(L"Audio", &index);
        } else if (task_ != nullptr) {
            AvRevertMmThreadCharacteristics(task_);
            task_ = nullptr;
        }
    }

  private:
    HANDLE task_{nullptr};
    bool active_{false};
};
} // namespace
#endif

void wait_room_worker(std::chrono::milliseconds duration) {
#ifdef _WIN32
    // A thread owns its timer. No global timer-resolution/priority changes and
    // no handle creation during room DSP; the worker is waiting for ring space.
    thread_local const RoomWaitTimer timer;
    if (timer.wait(duration)) {
        return;
    }
#endif
    // Older Windows versions or exhausted kernel resources retain a non-spinning
    // fallback. Realtime capacity must be measured on that host.
    std::this_thread::sleep_for(duration);
}

void set_room_worker_active(bool active) {
#ifdef _WIN32
    thread_local RoomAudioTask task;
    task.set_active(active);
#else
    static_cast<void>(active);
#endif
}

} // namespace mradm::realtime
