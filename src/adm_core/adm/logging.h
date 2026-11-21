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

} // namespace mradm
