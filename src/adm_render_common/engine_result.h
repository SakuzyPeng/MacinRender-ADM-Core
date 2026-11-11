#pragma once

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "adm/errors.h"
#include "adm/logging.h"

#include "engine_ffi.h"

namespace mradm::render_common {

[[nodiscard]] inline std::string engine_text(MradmAdmText text) {
    return text.len == 0 ? std::string{} : std::string{reinterpret_cast<const char*>(text.data), text.len};
}

[[nodiscard]] inline MradmAdmText engine_text(std::string_view text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

// One mradm-engine request resolution: its status and log lines.
class EngineOutcome {
  public:
    // `call` passes out() and status() to the FFI function; a non-zero return is a broken contract.
    template <class Call> explicit EngineOutcome(Call&& call) {
        MradmEngineResult* raw = nullptr;
        if (call(&raw, &status_) != 0 || raw == nullptr) {
            throw std::logic_error("mradm-engine request resolution failed");
        }
        result_.reset(raw);
    }

    // The log lines in order; an empty module falls back to `module`. `skip` names a payload module.
    void emit(LogSink& logs, std::string_view module, std::string_view skip = {}) const {
        for (const auto& line : lines()) {
            const auto name = engine_text(line.module);
            if (!skip.empty() && name == skip) {
                continue;
            }
            logs.log(static_cast<LogLevel>(line.level),
                     name.empty() ? module : std::string_view{name},
                     engine_text(line.message));
        }
    }

    [[nodiscard]] std::span<const MradmEngineLog> lines() const noexcept { return {status_.logs, status_.logs_len}; }

    [[nodiscard]] bool ok() const noexcept { return status_.code == 0; }

    [[nodiscard]] Error error() const {
        return {static_cast<ErrorCode>(status_.code), engine_text(status_.message), engine_text(status_.context)};
    }

  private:
    struct Destroy {
        void operator()(MradmEngineResult* result) const noexcept { mradm_engine_result_destroy(result); }
    };
    std::unique_ptr<MradmEngineResult, Destroy> result_;
    MradmEngineStatus status_{};
};

} // namespace mradm::render_common
