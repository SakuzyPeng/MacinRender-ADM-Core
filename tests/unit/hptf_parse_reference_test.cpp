// Differential test: Rust AutoEq ParametricEQ parsing against the frozen C++ parser.
// Results must match exactly: bands bit for bit, or the same error code, message and context.
// Texts with an underflowing number (nonzero literal rounding to zero or a subnormal) are
// excluded: the old stream extraction accepted them on libstdc++ but failed them on libc++ and
// MSVC. The Rust parser rejects them everywhere, which differential_underflow asserts directly.
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../reference/hptf_parse/legacy.h"
#include "hptf_eq.h"

namespace {
namespace legacy = mradm::hptf_parse_legacy;

class Random {
  public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }
    std::size_t below(std::size_t limit) noexcept { return limit == 0U ? 0U : next() % limit; }
    template <class T> const T& pick(const std::vector<T>& values) noexcept { return values[below(values.size())]; }

  private:
    std::uint64_t state_;
};

struct Counts {
    std::size_t compared{0};
    std::size_t parsed{0};
    std::size_t platform_dependent{0};
    std::vector<std::string> mismatches;
};
Counts& counts() {
    static Counts value;
    return value;
}

std::string escaped(std::string_view text) {
    std::string out;
    constexpr std::string_view digits = "0123456789abcdef";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20U && u < 0x7fU && c != '\\') {
            out += c;
        } else {
            out.append("\\x").append(1, digits[u >> 4U]).append(1, digits[u & 15U]);
        }
    }
    return out;
}

bool same_double(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// A nonzero decimal literal whose value rounds below the smallest normal double.
bool underflow_literal(std::string_view token) {
    const auto exponent = token.find_first_of("eE");
    const auto mantissa = token.substr(0, exponent);
    if (std::ranges::none_of(mantissa, [](char c) { return c >= '1' && c <= '9'; })) {
        return false;
    }
    const std::string copy{token};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(value) &&
           std::abs(value) < std::numeric_limits<double>::min();
}

bool has_underflow_literal(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && space(text[i])) {
            ++i;
        }
        const auto start = i;
        while (i < text.size() && !space(text[i])) {
            ++i;
        }
        if (i > start && underflow_literal(text.substr(start, i - start))) {
            return true;
        }
    }
    return false;
}

void compare(std::string_view text) {
    if (has_underflow_literal(text)) {
        ++counts().platform_dependent;
        return;
    }
    ++counts().compared;
    const auto expected = legacy::parse_parametric_eq(text);
    const auto actual = mradm::render_common::parse_parametric_eq(text);
    bool failed = false;
    const auto fail = [&](std::string_view what) {
        if (!failed && counts().mismatches.size() < 20U) {
            std::string message{what};
            message.append(" differs for \"").append(escaped(text)).append("\"");
            counts().mismatches.push_back(std::move(message));
        } else if (!failed) {
            counts().mismatches.emplace_back();
        }
        failed = true;
    };
    if (!expected || !actual) {
        if (expected.has_value() != actual.has_value()) {
            fail(expected ? "acceptance (legacy parsed)" : "acceptance (Rust parsed)");
            return;
        }
        const auto& e = expected.error();
        const auto& a = actual.error();
        if (e.code != a.code || e.message != a.message || e.context != a.context) {
            fail("error");
        }
        return;
    }
    ++counts().parsed;
    if (!same_double(expected->preamp_db, actual->preamp_db) || expected->name != actual->name ||
        expected->bands.size() != actual->bands.size()) {
        fail("profile");
        return;
    }
    for (std::size_t i = 0; i < expected->bands.size(); ++i) {
        const auto& e = expected->bands[i];
        const auto& a = actual->bands[i];
        if (e.type != a.type || e.enabled != a.enabled || !same_double(e.fc_hz, a.fc_hz) ||
            !same_double(e.gain_db, a.gain_db) || !same_double(e.q, a.q)) {
            fail("band " + std::to_string(i + 1));
        }
    }
}

const std::string k_profile = "Preamp: -6.2 dB\n"
                              "Filter 1: ON LSC Fc 105 Hz Gain 2.5 dB Q 0.70\n"
                              "Filter 2: ON PK Fc 210.5 Hz Gain -3.1 dB Q 1.41\n"
                              "Filter 3: OFF HSC Fc 8000 Hz Gain 4 dB Q 0.7\n"
                              "Filter 4: ON LP Fc 18000 Hz Q 0.71\n"
                              "Filter 5: ON NO Fc 6000 Hz Q 4\n";

std::vector<std::string> fixed_cases() {
    std::vector<std::string> cases{k_profile,
                                   "",
                                   "\n",
                                   "# comment only\n",
                                   "Preamp: 0 dB",
                                   "Preamp:",
                                   "Preamp: bad dB",
                                   "preamp 1e-1",
                                   "PREAMP: +.5",
                                   "Preamp: -0",
                                   "Filter 1: ON",
                                   "Filter 1: ON ZZZ Fc 1",
                                   "Filter 1: ON PK",
                                   "Filter 1: ON PK Fc",
                                   "Filter 1: ON PK Fc abc Hz",
                                   "Filter 1: ON PK Fc 100 Gain",
                                   "Filter 1: ON PK Fc 100 Q 0",
                                   "Filter 1: ON PK Fc 0 Gain 1",
                                   "Filter Q 3: ON PK Fc 10",
                                   "Filter Q 3 : ON PK Fc 10",
                                   "Filter 1: on pk fc 10 GAIN 1 q 2",
                                   "Filter\v1:\fON\vBP\vFc\v7",
                                   "\r\n  # c\r\n  Preamp: -1.5 dB  \r\nFilter 1: ON PEQ Fc 1000 Hz Gain 3 dB Q 1\r\n",
                                   "\xEF\xBB\xBF" + k_profile,
                                   "Filter 1: ON MODAL Fc 50 Hz Gain 1 dB Q 9 Fc 70",
                                   std::string{"Preamp: 1\0x\n", 12},
                                   "Preamp: 1\xc2\xa0"};
    for (const std::string_view type : {"PK",
                                        "PEQ",
                                        "MODAL",
                                        "LSC",
                                        "LS",
                                        "LSQ",
                                        "HSC",
                                        "HS",
                                        "HSQ",
                                        "LP",
                                        "LPQ",
                                        "HP",
                                        "HPQ",
                                        "BP",
                                        "NO",
                                        "lsq",
                                        "Pk"}) {
        cases.push_back(std::string{"Filter 1: ON "} + std::string{type} + " Fc 1000 Hz Gain 1 dB Q 1");
    }
    for (const std::string_view number : {"1.",
                                          ".5",
                                          "+2",
                                          "-0",
                                          "1E2",
                                          "1e-400",
                                          "1e-310",
                                          "1e",
                                          "1e+",
                                          ".",
                                          "+",
                                          "1,5",
                                          "1.2.3",
                                          "0x10",
                                          "0x1p3",
                                          "inf",
                                          "-inf",
                                          "+inf",
                                          "infinity",
                                          "NaN",
                                          "nan(1)",
                                          "1e400",
                                          "1Hz",
                                          "--1",
                                          "+-1",
                                          "1e5x",
                                          "007",
                                          "1e0005",
                                          "9007199254740993",
                                          "123456789012345678901234567890.123456789e-10",
                                          "4.9e-324",
                                          "2.2250738585072011e-308",
                                          "1.7976931348623157e308",
                                          "1.7976931348623159e308"}) {
        cases.push_back(std::string{"Preamp: "} + std::string{number} + " dB");
        cases.push_back(std::string{"Filter 1: ON PK Fc "} + std::string{number} + " Hz Gain 1 dB Q 1");
        cases.push_back(std::string{"Filter 1: ON PK Fc 100 Hz Gain 1 dB Q "} + std::string{number});
    }
    std::string many = "Preamp: 0\n";
    std::string too_many = many;
    for (int i = 1; i <= 40; ++i) {
        const auto line = "Filter " + std::to_string(i) + ": " + (i <= 32 ? "ON" : "OFF") + " PK Fc " +
                          std::to_string(i * 100) + " Hz Gain 1 dB Q 1\n";
        many += line;
        too_many += "Filter " + std::to_string(i) + ": ON PK Fc " + std::to_string(i * 100) + " Hz Gain 1 dB Q 1\n";
    }
    cases.push_back(many);
    cases.push_back(too_many);
    return cases;
}

void differential_fixed() {
    for (const auto& text : fixed_cases()) {
        compare(text);
        for (std::size_t size = 0; size < text.size(); ++size) {
            compare(std::string_view{text}.substr(0, size));
        }
    }
}

// Lines assembled from AutoEq vocabulary, so most samples reach the band grammar.
void differential_tokens(Random& random) {
    const std::vector<std::string> words{
        "Filter", "filter", "1:",  "2:",  "ON", "OFF",  "on", "PK", "LSC", "HSQ",     "LP",     "HP",
        "BP",     "NO",     "ZZZ", "Fc",  "fc", "Gain", "Q",  "Hz", "dB",  "Preamp:", "Preamp", "#",
        "1000",   "-3.5",   ".5",  "1e3", "1e", "abc",  "0",  "-0", "inf", "1e400",   "+2",     "0x10"};
    const std::vector<std::string> spaces{" ", "  ", "\t", "\v", "\f", "\r"};
    for (int round = 0; round < 100000; ++round) {
        std::string text;
        const auto lines = 1U + random.below(5);
        for (std::size_t line = 0; line < lines; ++line) {
            if (random.below(3) != 0U) {
                text += random.below(2) == 0U ? "Filter 1: ON PK " : "Preamp: ";
            }
            const auto count = random.below(9);
            for (std::size_t word = 0; word < count; ++word) {
                text += random.pick(words);
                text += random.pick(spaces);
            }
            text += random.below(4) == 0U ? "\r\n" : "\n";
        }
        compare(text);
    }
}

void differential_bytes(Random& random) {
    const auto seeds = fixed_cases();
    constexpr std::string_view alphabet = "Filter ONPKFcGainQHzdB0123456789.-+eE:#\t\n\r\v\f \x80\xc2\xa0\xff";
    for (int round = 0; round < 100000; ++round) {
        auto text = random.pick(seeds);
        const auto edits = 1U + random.below(4);
        for (std::size_t edit = 0; edit < edits; ++edit) {
            const auto at = random.below(text.size() + 1U);
            const char c =
                random.below(16) == 0U ? static_cast<char>(random.next()) : alphabet.at(random.below(alphabet.size()));
            switch (random.below(3)) {
            case 0:
                if (at < text.size()) {
                    text.at(at) = c;
                }
                break;
            case 1:
                if (at < text.size()) {
                    text.erase(at, 1U);
                }
                break;
            default:
                text.insert(at, 1U, c);
                break;
            }
        }
        compare(text);
    }
}

// Rust rejects underflowing numbers on every platform, wherever they appear.
void differential_underflow() {
    for (const std::string_view literal : {"1e-400", "-1e-400", "1e-310", "4.9e-324", "0.1e-320"}) {
        for (const auto& line : {std::string{"Preamp: "} + std::string{literal} + " dB",
                                 std::string{"Filter 1: ON PK Fc 100 Hz Gain "} + std::string{literal},
                                 std::string{"Filter 1: ON PK Fc 100 Hz Q "} + std::string{literal}}) {
            if (!has_underflow_literal(line) || mradm::render_common::parse_parametric_eq(line)) {
                throw std::runtime_error("underflowing number must be rejected: " + line);
            }
        }
    }
    for (const std::string_view zero : {"0e-400", "0.000e-999", "-0.0"}) {
        const auto line = std::string{"Preamp: "} + std::string{zero};
        const auto parsed = mradm::render_common::parse_parametric_eq(line);
        if (has_underflow_literal(line) || !parsed || parsed->preamp_db != 0.0) {
            throw std::runtime_error("an explicit zero must still parse: " + line);
        }
    }
}

} // namespace

int main() {
    try {
        differential_fixed();
        Random random(0x5eed0004U);
        differential_tokens(random);
        differential_bytes(random);
        differential_underflow();
        std::cout << "HpTF ParametricEQ differential: " << counts().compared << " texts, " << counts().parsed
                  << " parsed, " << counts().platform_dependent << " underflow texts excluded\n";
        if (!counts().mismatches.empty()) {
            for (const auto& mismatch : counts().mismatches) {
                if (!mismatch.empty()) {
                    std::cerr << "MISMATCH: " << mismatch << '\n';
                }
            }
            throw std::runtime_error(std::to_string(counts().mismatches.size()) + " mismatching texts");
        }
        if (counts().parsed < 1000U) {
            throw std::runtime_error("corpus must exercise successful parses");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
