#include "wav_output.h"

#include "audio_io_internal.h"

namespace mradm::engine {

// The layout table and the ADM metadata are built in Rust (PCM consistency 4.8).
Result<void> finalize_rendered_wav(const std::string& path,
                                   const std::string& output_layout,
                                   const std::stop_token& cancel_token,
                                   ProgressSink* progress,
                                   WavLayoutProfile profile) {
    return audio::finalize_wav_output(
        path, output_layout, profile == WavLayoutProfile::speaker_rerender, cancel_token, progress);
}

} // namespace mradm::engine
