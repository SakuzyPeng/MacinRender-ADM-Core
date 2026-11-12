// The output devices as the engine opens and drives them: miniaudio (hardware and its null
// backend) and the platform's system-spatial sink, as MradmDeviceTable records.
#pragma once
#include "../adm_render_common/engine_ffi.h"
#ifdef __cplusplus
#include <memory>

#include "audio_output_device.h"

extern "C" {
#endif

// The process-lifetime table of this build's devices.
const MradmDeviceIslands* mradm_device_islands(void);

#ifdef __cplusplus
}

namespace mradm::realtime {
// The table of a C++ device (the engine takes it over); also how tests inject theirs.
[[nodiscard]] MradmDeviceTable device_table(std::unique_ptr<IAudioOutputDevice> device);
} // namespace mradm::realtime
#endif
