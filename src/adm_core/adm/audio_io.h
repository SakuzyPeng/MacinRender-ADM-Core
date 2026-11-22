#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "adm/errors.h"

#include "../../adm_audio/wav_ffi.h"

namespace mradm::audio {

// RIFF/RF64/BW64 input decoded to interleaved float32. Read and seek failures carry the input path.
class RenderInputReader {
  public:
    static Result<std::unique_ptr<RenderInputReader>> open(const std::string& path);
    [[nodiscard]] const MradmWavInfo& info() const noexcept { return info_; }
    Result<uint64_t> read(float* out, uint64_t frames);
    Result<void> seek_frame(uint64_t frame);

  private:
    explicit RenderInputReader(std::string path);
    std::string path_;
    MradmWavInfo info_{};
    std::unique_ptr<MradmWavReader, void (*)(MradmWavReader*)> handle_;
};

// Interleaved float32 output. A .caf extension selects CAF with the supplied layout. Other paths use RF64 WAVE.
class WriterHandle {
  public:
    static Result<WriterHandle>
    open(const std::string& path, uint32_t channels, uint32_t sample_rate, const std::string& layout_id = {});

    // Whole frames written, or zero on failure. Samples outside [-1, 1] retain their headroom.
    uint64_t write(const float* samples, uint64_t frame_count);
    // Returns the first finish result. Destruction finishes an open writer on a best-effort basis.
    Result<void> finish();

  private:
    WriterHandle(std::string path, uint32_t channels);
    std::string path_;
    uint32_t channels_;
    std::unique_ptr<MradmOutputWriter, void (*)(MradmOutputWriter*)> handle_;
};

} // namespace mradm::audio
