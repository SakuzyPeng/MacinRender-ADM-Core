#pragma once

// Opt-in, non-installed phase-0 probes. Normal builds compile these calls away.
// Each process writes only the first value of a named checkpoint; direction-dependent
// checkpoints use their quantised grid index as a key, independent of worker scheduling.
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
#include <bit>
#include <charconv>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#endif

namespace mradm::consistency {

template <typename T> inline void dump(const std::string& name, std::span<const T> values) {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    const char* directory = std::getenv("MR_ADM_TRACE_DIR");
    if (directory == nullptr || *directory == '\0') {
        return;
    }
    static std::mutex mutex;
    static std::set<std::string> written;
    const std::lock_guard lock(mutex);
    const auto path = std::filesystem::path(directory) / name;
    if (!written.insert(path.string()).second) {
        return;
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    // Explicit little-endian words, including signed zero. No native struct padding or text floats.
    auto word = [&out](auto bits) {
        for (std::size_t i = 0; i < sizeof(bits); ++i) {
            out.put(static_cast<char>((bits >> (i * 8U)) & 0xffU));
        }
    };
    for (const auto& value : values) {
        if constexpr (std::is_same_v<T, float>) {
            word(std::bit_cast<std::uint32_t>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            word(std::bit_cast<std::uint64_t>(value));
        } else if constexpr (std::is_same_v<T, std::complex<float>>) {
            word(std::bit_cast<std::uint32_t>(value.real()));
            word(std::bit_cast<std::uint32_t>(value.imag()));
        } else {
            static_assert(std::is_same_v<T, int>);
            word(static_cast<std::uint32_t>(value));
        }
    }
#else
    (void) name;
    (void) values;
#endif
}

template <typename T> inline void dump(const std::string& name, const std::vector<T>& values) {
    dump(name, std::span<const T>(values));
}

template <typename T> inline void dump(const std::string& name, std::initializer_list<T> values) {
    dump(name, std::span<const T>(values.begin(), values.size()));
}

inline std::size_t count_override(const char* variable, std::size_t fallback) {
#ifdef MR_ADM_CONSISTENCY_DIAGNOSTICS
    const char* value = std::getenv(variable);
    if (value != nullptr) {
        const std::string text(value);
        std::size_t count = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || count < 1U || count > 256U) {
            throw std::runtime_error(std::string(variable) + " must be an integer in [1, 256]");
        }
        return count;
    }
#else
    (void) variable;
#endif
    return fallback;
}

} // namespace mradm::consistency
