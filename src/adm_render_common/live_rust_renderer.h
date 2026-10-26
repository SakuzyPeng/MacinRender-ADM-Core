#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "live_scene_renderer.h"
#include "routing_ffi.h"

namespace mradm::live_scene {

// Rust error text: fixed wording plus the input bytes, so `extra` (their size) always fits; the
// context likewise with `context_extra`.
class RustError {
  public:
    explicit RustError(std::size_t extra = 0, std::size_t context_extra = 0)
        : message_(extra + 512), context_(context_extra + 512) {}
    uint8_t* message() noexcept { return message_.data(); }
    uint8_t* context() noexcept { return context_.data(); }
    std::size_t* message_len() noexcept { return &(message_len_ = message_.size()); }
    std::size_t* context_len() noexcept { return &(context_len_ = context_.size()); }
    [[nodiscard]] tl::unexpected<Error> error(int32_t code) const {
        return make_error(static_cast<ErrorCode>(code),
                          std::string{reinterpret_cast<const char*>(message_.data()), message_len_},
                          std::string{reinterpret_cast<const char*>(context_.data()), context_len_});
    }

  private:
    std::vector<uint8_t> message_;
    std::vector<uint8_t> context_;
    std::size_t message_len_{0};
    std::size_t context_len_{0};
};

// The ILiveSceneRenderer of a Rust mradm-render Scene stream renderer, which owns generations,
// frame staging, warnings and the checkpoints named `<scene renderer key><trace_suffix>-*`. A room
// renderer also hands over its room control.
[[nodiscard]] std::unique_ptr<ILiveSceneRenderer> adopt_live_renderer(MradmRenderLive* handle,
                                                                      uint32_t sample_rate,
                                                                      DiagnosticSink diagnostics,
                                                                      std::string trace_suffix,
                                                                      std::shared_ptr<IRoomControl> room = {});

} // namespace mradm::live_scene
