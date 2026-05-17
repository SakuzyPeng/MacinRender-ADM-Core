#pragma once

#include <string>

#include <tl/expected.hpp>

namespace mradm {

enum class ErrorCode {
    ok = 0,
    invalid_argument = 1,
    unsupported = 2,
    io_error = 3,
    render_failed = 4,
    cancelled = 5,
    internal_error = 6,
};

// Numeric values must stay in sync with adm_error_code_t in c_api.h
static_assert(static_cast<int>(ErrorCode::ok) == 0);
static_assert(static_cast<int>(ErrorCode::invalid_argument) == 1);
static_assert(static_cast<int>(ErrorCode::unsupported) == 2);
static_assert(static_cast<int>(ErrorCode::io_error) == 3);
static_assert(static_cast<int>(ErrorCode::render_failed) == 4);
static_assert(static_cast<int>(ErrorCode::cancelled) == 5);
static_assert(static_cast<int>(ErrorCode::internal_error) == 6);

struct Error {
    ErrorCode code{ErrorCode::ok};
    std::string message;
    std::string context;

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::ok; }
};

template <typename T> using Result = tl::expected<T, Error>;

inline tl::unexpected<Error> make_error(ErrorCode code, std::string message, std::string context = {}) {
    return tl::unexpected{Error{code, std::move(message), std::move(context)}};
}

} // namespace mradm
