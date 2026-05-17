#pragma once

#include <string_view>

namespace mradm {

enum class LogLevel {
    debug,
    info,
    warning,
    error,
};

class LogSink {
  public:
    virtual ~LogSink() = default;
    virtual void log(LogLevel level, std::string_view module, std::string_view message) = 0;
};

class NullLogSink final : public LogSink {
  public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

} // namespace mradm
