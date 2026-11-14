#pragma once

#include <exception>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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

// The failed side of a Result (6.5 C: the project's own, replacing tl::expected; only the
// operations the islands and tests use).
template <typename E> class unexpected {
  public:
    explicit unexpected(E error) : error_(std::move(error)) {}
    [[nodiscard]] const E& error() const& noexcept { return error_; }
    [[nodiscard]] E& error() & noexcept { return error_; }
    [[nodiscard]] E&& error() && noexcept { return std::move(error_); }

  private:
    E error_;
};
template <typename E> unexpected(E) -> unexpected<E>;

// Thrown by Result::value() without a value, as tl::bad_expected_access was.
class BadResultAccess : public std::exception {
  public:
    [[nodiscard]] const char* what() const noexcept override { return "bad Result access"; }
};

template <typename T> class [[nodiscard]] Result {
  public:
    Result()
        requires std::is_default_constructible_v<T>
        : state_(std::in_place_index<0>) {}
    template <typename U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Result>)
    // cppcheck-suppress noExplicitConstructor ; implicit, as in tl::expected
    explicit(!std::is_convertible_v<U&&, T>) Result(U&& value)
        : state_(std::in_place_index<0>, std::forward<U>(value)) {}
    // cppcheck-suppress noExplicitConstructor ; implicit, as in tl::expected
    template <typename G> Result(const unexpected<G>& failure) : state_(std::in_place_index<1>, failure.error()) {}
    template <typename G>
    // cppcheck-suppress noExplicitConstructor ; implicit, as in tl::expected
    Result(unexpected<G>&& failure) : state_(std::in_place_index<1>, std::move(failure).error()) {}

    [[nodiscard]] bool has_value() const noexcept { return state_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return checked(*this); }
    [[nodiscard]] const T& value() const& { return checked(*this); }
    [[nodiscard]] T&& value() && { return std::move(checked(*this)); }
    [[nodiscard]] T& operator*() & noexcept { return *std::get_if<0>(&state_); }
    [[nodiscard]] const T& operator*() const& noexcept { return *std::get_if<0>(&state_); }
    [[nodiscard]] T&& operator*() && noexcept { return std::move(*std::get_if<0>(&state_)); }
    [[nodiscard]] T* operator->() noexcept { return std::get_if<0>(&state_); }
    [[nodiscard]] const T* operator->() const noexcept { return std::get_if<0>(&state_); }
    [[nodiscard]] Error& error() & noexcept { return *std::get_if<1>(&state_); }
    [[nodiscard]] const Error& error() const& noexcept { return *std::get_if<1>(&state_); }
    [[nodiscard]] Error&& error() && noexcept { return std::move(*std::get_if<1>(&state_)); }

    template <typename U> [[nodiscard]] T value_or(U&& fallback) const& {
        return has_value() ? **this : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U> [[nodiscard]] T value_or(U&& fallback) && {
        return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(fallback));
    }
    // As tl::expected: equal to a value only when holding an equal value.
    template <typename U> [[nodiscard]] friend bool operator==(const Result& result, const U& other) {
        return result.has_value() && *result == other;
    }
    template <typename... Args> T& emplace(Args&&... args) {
        return state_.template emplace<0>(std::forward<Args>(args)...);
    }

  private:
    template <typename Self> static auto& checked(Self& self) {
        if (!self.has_value()) {
            throw BadResultAccess{};
        }
        return *std::get_if<0>(&self.state_);
    }

    std::variant<T, Error> state_;
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() = default;
    // cppcheck-suppress noExplicitConstructor ; implicit, as in tl::expected
    template <typename G> Result(const unexpected<G>& failure) : error_(failure.error()) {}
    // cppcheck-suppress noExplicitConstructor ; implicit, as in tl::expected
    template <typename G> Result(unexpected<G>&& failure) : error_(std::move(failure).error()) {}

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }
    void value() const {
        if (error_) {
            throw BadResultAccess{};
        }
    }
    void operator*() const noexcept {}
    [[nodiscard]] Error& error() & noexcept { return *error_; }
    [[nodiscard]] const Error& error() const& noexcept { return *error_; }
    [[nodiscard]] Error&& error() && noexcept { return std::move(*error_); }
    void emplace() noexcept { error_.reset(); }

  private:
    std::optional<Error> error_;
};

inline unexpected<Error> make_error(ErrorCode code, std::string message, std::string context = {}) {
    return unexpected{Error{code, std::move(message), std::move(context)}};
}

} // namespace mradm
