#pragma once

#include <chrono>

namespace mradm::realtime {

// Room output-queue backoff only, called outside DSP and never from a device
// callback. Windows Sleep(1) can consume about 16 ms of the tracking lookahead.
void wait_room_worker(std::chrono::milliseconds duration);
// Called at a streaming worker's backend boundary, outside the DSP operation.
// Releases the scheduling task when the worker leaves room rendering.
void set_room_worker_active(bool active);

} // namespace mradm::realtime
