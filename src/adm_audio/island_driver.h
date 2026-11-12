// The caller's side of a render island as the C++ islands use it: the engine's texts, the
// caller's cancellation as a stop token, raw progress and log lines through MradmIslandContext,
// and the reply's error texts.
#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "adm/errors.h"
#include "adm/logging.h"
#include "adm/progress.h"

#include "../adm_render_common/engine_ffi.h"

namespace mradm::island {

[[nodiscard]] inline std::string text(MradmAdmText t) {
    return t.data == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(t.data), t.len};
}

[[nodiscard]] inline MradmAdmText view(std::string_view s) noexcept {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// The native spelling of a UTF-8 path from the engine.
[[nodiscard]] inline std::string native(MradmAdmText t) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(t.data), t.len);
    return std::filesystem::path(bytes).string();
}

// The caller's cancellation as a stop token, polled while the island runs.
class Cancel {
  public:
    explicit Cancel(const MradmIslandContext& context) : context_(context) {
        if (!poll()) {
            watcher_ = std::jthread([this](const std::stop_token& done) {
                while (!done.stop_requested() && !poll()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
        }
    }
    Cancel(const Cancel&) = delete;
    Cancel& operator=(const Cancel&) = delete;
    Cancel(Cancel&&) = delete;
    Cancel& operator=(Cancel&&) = delete;
    ~Cancel() = default;
    [[nodiscard]] std::stop_token token() const { return source_.get_token(); }

  private:
    bool poll() {
        if (context_.cancelled == nullptr || context_.cancelled(context_.user) == 0U) {
            return false;
        }
        source_.request_stop();
        return true;
    }
    MradmIslandContext context_;
    std::stop_source source_;
    std::jthread watcher_; // Joined before the stop source goes.
};

// Raw progress to the engine (which maps it into the render's range).
class Progress final : public ProgressSink {
  public:
    explicit Progress(const MradmIslandContext& context) : context_(context) {}
    void on_progress(const ProgressEvent& e) override {
        if (context_.progress != nullptr) {
            const MradmEngineProgress raw{static_cast<uint32_t>(e.stage),
                                          static_cast<uint32_t>(e.operation),
                                          e.fraction,
                                          e.stage_fraction,
                                          e.current_frame,
                                          e.total_frames,
                                          view(e.message)};
            context_.progress(context_.user, &raw);
        }
    }

  private:
    MradmIslandContext context_;
};

// Log lines to the caller.
class Logs final : public LogSink {
  public:
    explicit Logs(const MradmIslandContext& context) : context_(context) {}
    void log(LogLevel level, std::string_view module, std::string_view message) override {
        if (context_.log != nullptr) {
            context_.log(context_.user, static_cast<uint32_t>(level), view(module), view(message));
        }
    }

  private:
    MradmIslandContext context_;
};

// The reply's texts live until the next island on this thread.
inline void fail(MradmEngineIslandReply& reply, const Error& error) {
    thread_local std::string message;
    thread_local std::string context;
    message = error.message;
    context = error.context;
    reply.code = static_cast<int32_t>(error.code);
    reply.message = view(message);
    reply.context = view(context);
}

} // namespace mradm::island
