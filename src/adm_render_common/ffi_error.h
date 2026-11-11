#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "adm/errors.h"

namespace mradm::live_scene {

// Error text of a private Rust FFI call: fixed wording plus the input bytes, so `extra` (their size)
// always fits; the context likewise with `context_extra`.
class FfiError {
  public:
    explicit FfiError(std::size_t extra = 0, std::size_t context_extra = 0)
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

} // namespace mradm::live_scene
