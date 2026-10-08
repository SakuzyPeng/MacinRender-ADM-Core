// Differential test: Rust mradm-osc against the frozen C++ PoseBridge decoder.
// Every accepted/rejected decision and every decoded field must match bit for bit.
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../reference/osc_protocol/legacy.h"
#include "osc_head_tracking_protocol.h"

namespace {
namespace legacy = mradm::osc_protocol_legacy;
namespace current = mradm::realtime;
using Timing = mradm::HeadTrackingTiming;
using Bytes = std::vector<std::byte>;

void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string{message});
    }
}

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

  private:
    std::uint64_t state_;
};

void append_text(Bytes& bytes, std::string_view value) {
    std::ranges::transform(value, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
    bytes.push_back(std::byte{0});
    while (bytes.size() % 4U != 0U) {
        bytes.push_back(std::byte{0});
    }
}
void append_integer(Bytes& bytes, std::uint64_t value, int width) {
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 255U));
    }
}

Timing fixture() {
    Timing t;
    t.protocol_version = 3;
    t.instance_id = 123;
    t.source_session_id = 456;
    t.source_sequence = 1;
    t.tx_sequence = 1;
    t.reference_epoch = 1;
    t.metadata_revision = 1;
    return t;
}

Bytes pose(bool quaternion, const Timing& t, std::span<const float> values, std::string_view source = "head") {
    Bytes bytes;
    append_text(bytes, quaternion ? "/posebridge/quaternion" : "/posebridge/euler");
    append_text(bytes, quaternion ? ",ishhhhhhhhihhffff" : ",ishhhhhhhhihhfff");
    append_integer(bytes, t.protocol_version, 4);
    append_text(bytes, source);
    for (const auto v : {t.instance_id,
                         t.source_session_id,
                         t.source_sequence,
                         t.tx_sequence,
                         t.reference_epoch,
                         t.metadata_revision,
                         t.source_received_ns,
                         t.source_age_at_send_ns}) {
        append_integer(bytes, v, 8);
    }
    append_integer(bytes, t.sample_time_kind, 4);
    append_integer(bytes, t.sample_time_ms, 8);
    append_integer(bytes, t.sample_clock_epoch, 8);
    for (const float v : values) {
        append_integer(bytes, std::bit_cast<std::uint32_t>(v), 4);
    }
    return bytes;
}

Bytes telemetry(std::string_view json, bool info) {
    Bytes bytes;
    append_text(bytes, info ? "/posebridge/info" : "/posebridge/status");
    append_text(bytes, ",s");
    append_text(bytes, json);
    return bytes;
}

const std::string k_info =
    R"({"schema":3,"kind":"info","source_id":"head","instance_id":"123","session_id":"456",)"
    R"("reference_epoch":"1","metadata_revision":"1","message_seq":"7","descriptor":{"source_id":"head",)"
    R"("instance_id":"123","session_id":"456","reference_epoch":"1","metadata_revision":"1",)"
    R"("coordinate_profile":"posebridge.yxz.v1"}})";
const std::string k_status = R"({"schema":3,"kind":"status","source_id":"head","instance_id":"123","session_id":"456",)"
                             R"("reference_epoch":"1","metadata_revision":"1","message_seq":"7",)"
                             R"("status":{"session_id":"456","session_samples":"9","state":"active"}})";

std::string replace_first(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

struct Counts {
    std::size_t compared{0};
    std::size_t accepted{0};
};
Counts& counts() {
    static Counts value;
    return value;
}

std::string hex(std::span<const std::byte> data) {
    std::string out;
    constexpr std::string_view digits = "0123456789abcdef";
    for (const auto b : data) {
        const auto v = std::to_integer<unsigned>(b);
        out += digits[v >> 4U];
        out += digits[v & 15U];
    }
    return out;
}

bool same_timing(const Timing& a, const Timing& b) {
    return a.protocol_version == b.protocol_version && a.sample_time_kind == b.sample_time_kind &&
           a.instance_id == b.instance_id && a.tx_sequence == b.tx_sequence && a.reference_epoch == b.reference_epoch &&
           a.metadata_revision == b.metadata_revision && a.source_age_at_send_ns == b.source_age_at_send_ns &&
           a.source_session_id == b.source_session_id && a.source_sequence == b.source_sequence &&
           a.source_received_ns == b.source_received_ns && a.sample_time_ms == b.sample_time_ms &&
           a.sample_clock_epoch == b.sample_clock_epoch;
}

template <std::size_t N> bool same_bits(const std::array<float, N>& a, const std::array<float, N>& b) {
    for (std::size_t i = 0; i < N; ++i) {
        if (std::bit_cast<std::uint32_t>(a.at(i)) != std::bit_cast<std::uint32_t>(b.at(i))) {
            return false;
        }
    }
    return true;
}

void compare(std::span<const std::byte> data) {
    ++counts().compared;
    const auto legacy_result = legacy::decode_head_tracking_osc(data);
    const auto rust_result = current::decode_head_tracking_osc(data);
    const auto fail = [&](std::string_view what) {
        std::string message{what};
        message.append(" differs for datagram ").append(hex(data));
        throw std::runtime_error(message);
    };
    if (!legacy_result || !rust_result) {
        if (legacy_result.has_value() != rust_result.has_value()) {
            fail(legacy_result ? "acceptance (legacy accepted)" : "acceptance (Rust accepted)");
        }
        return;
    }
    ++counts().accepted;
    const auto& expected = *legacy_result;
    const auto& actual = *rust_result;
    if (static_cast<int>(expected.kind) != static_cast<int>(actual.kind)) {
        fail("kind");
    }
    if (!same_bits(expected.orientation.quaternion_xyzw, actual.orientation.quaternion_xyzw) ||
        !same_bits(expected.orientation.euler_deg, actual.orientation.euler_deg)) {
        fail("orientation");
    }
    if (!same_timing(expected.timing, actual.timing)) {
        fail("timing");
    }
    if (expected.source_id != actual.source_id || expected.json != actual.json) {
        fail("strings");
    }
    if (expected.message_sequence != actual.message_sequence || expected.reported_samples != actual.reported_samples ||
        expected.source_active != actual.source_active) {
        fail("telemetry fields");
    }
}

void compare(const Bytes& data) {
    compare(std::span<const std::byte>{data});
}

void compare_source_id(std::string_view value) {
    if (legacy::valid_source_id(value) != current::valid_source_id(value)) {
        throw std::runtime_error("valid_source_id differs for " + hex(std::as_bytes(std::span{value})));
    }
}

std::vector<std::string> json_edge_cases() {
    std::vector<std::string> cases{k_info, k_status};
    const auto with_extra = [](std::string_view extra) {
        std::string field{R"({"extra":)"};
        field.append(extra).append(",");
        return replace_first(k_info, "{", field);
    };
    for (int levels = 14; levels <= 18; ++levels) {
        const std::string open(static_cast<std::size_t>(levels), '[');
        const std::string close(static_cast<std::size_t>(levels), ']');
        for (const std::string_view inner : {"", "1", "{}", R"({"a":1})", R"("x")"}) {
            std::string nested{open};
            nested.append(inner).append(close);
            cases.push_back(with_extra(nested));
        }
    }
    for (const std::string_view extra : {R"("[[[[[[[[[[[[[[[[[[[[")",
                                         "-0",
                                         "1e400",
                                         "-1e400",
                                         "1e-400",
                                         "123456789012345678901234567890",
                                         "18446744073709551615",
                                         "18446744073709551616",
                                         "-9223372036854775809",
                                         "0.5",
                                         "true",
                                         "null",
                                         R"("\u0000")",
                                         R"("😀")",
                                         R"("\ud800")",
                                         R"("\udc00x")",
                                         "\"\x7f\"",
                                         "\"\xc2\x85\"",
                                         "\"\xff\"",
                                         "\"\xed\xa0\x80\"",
                                         "\"a\tb\"",
                                         R"({"":{}})",
                                         R"([1,])",
                                         "01",
                                         "+1",
                                         ".5",
                                         "1.",
                                         "NaN",
                                         "Infinity",
                                         "/*x*/1",
                                         R"(["\\"])",
                                         R"("\/")",
                                         R"("\x")",
                                         R"("\u12")",
                                         "\"\xf0\x9f\x98\x80\""}) {
        cases.push_back(with_extra(extra));
    }
    for (const std::string_view schema :
         {"-0", "3.0", "3e0", "\"3\"", "03", "4", "2", "1e400", "18446744073709551619"}) {
        cases.push_back(replace_first(k_info, R"("schema":3)", std::string{R"("schema":)"} + std::string{schema}));
    }
    cases.push_back(std::string{"\xEF\xBB\xBF"} + k_info);
    cases.push_back(std::string{"\xEF\xBB"} + k_info);
    cases.push_back(std::string{" \xEF\xBB\xBF"} + k_info);
    cases.push_back(" \t\r\n" + k_info + " \n");
    cases.push_back(k_info + "x");
    cases.push_back(k_info + k_info);
    cases.push_back(replace_first(k_info, R"("schema":3,)", R"("schema":3,"schema":4,)"));
    cases.push_back(replace_first(k_info, R"("schema":3,)", R"("schema":4,"schema":3,)"));
    cases.push_back(replace_first(k_info, R"("kind":"info")", R"("kind":"info","kind":7)"));
    cases.push_back(replace_first(k_info, R"("source_id":"head")", R"("source_id":"head")"));
    cases.push_back(replace_first(k_info, R"("source_id":"head")", R"("source_id":"　")"));
    cases.push_back(replace_first(k_info, R"("source_id":"head")", R"("source_id":"\u0085x")"));
    cases.push_back(replace_first(k_status, "active", "inspecting"));
    cases.push_back(replace_first(k_status, "active", "ACTIVE"));
    cases.push_back(replace_first(k_status, R"("session_samples":"9")", R"("session_samples":"0")"));
    cases.push_back(replace_first(k_status, R"("session_samples":"9")", R"("session_samples":9)"));
    cases.push_back(replace_first(k_info, R"("message_seq":"7")", R"("message_seq":"9223372036854775807")"));
    cases.push_back(replace_first(k_info, R"("message_seq":"7")", R"("message_seq":"9223372036854775808")"));
    cases.push_back(replace_first(k_info, R"("message_seq":"7")", R"("message_seq":"18446744073709551616")"));
    cases.emplace_back("[]");
    cases.emplace_back("3");
    cases.emplace_back("");
    return cases;
}

std::vector<Bytes> seed_corpus() {
    const auto t = fixture();
    std::vector<Bytes> seeds;
    for (const auto& json : json_edge_cases()) {
        seeds.push_back(telemetry(json, true));
        seeds.push_back(telemetry(json, false));
    }
    seeds.push_back(pose(false, t, std::array{30.0F, 20.0F, 10.0F}));
    seeds.push_back(pose(true, t, std::array{0.0F, 0.70710677F, 0.0F, 0.70710677F}));
    seeds.push_back(pose(true, t, std::array{0.0F, 0.0F, 0.0F, 0.0F}));
    seeds.push_back(pose(false, t, std::array{720.0F, -90.0F, 180.0F}));
    seeds.push_back(pose(false, t, std::array{std::bit_cast<float>(0x7fc00001U), 0.0F, 0.0F}));
    seeds.push_back(pose(false, t, std::array{0.0F, 0.0F, 0.0F}, "\xe8\x80\xb3\xe6\x9c\xba"));
    auto clock = t;
    clock.sample_time_kind = 1;
    clock.sample_clock_epoch = 1;
    clock.sample_time_ms = 1000;
    seeds.push_back(pose(false, clock, std::array{1.0F, 2.0F, 3.0F}));
    auto old = t;
    old.protocol_version = 2;
    seeds.push_back(pose(false, old, std::array{1.0F, 2.0F, 3.0F}));
    return seeds;
}

void compare_seeds_and_prefixes(const std::vector<Bytes>& seeds) {
    for (const auto& seed : seeds) {
        compare(seed);
        for (std::size_t size = 0; size < seed.size(); ++size) {
            compare(std::span<const std::byte>{seed.data(), size});
        }
    }
    compare(Bytes(8193, std::byte{0}));
    compare(Bytes{});
}

// Pose field sweep: every integer field around its validation boundaries.
void sweep_pose_fields(Random& random) {
    const auto t = fixture();
    constexpr std::array<std::uint64_t, 9> edges{
        0U, 1U, 2U, 499999999U, 500000000U, 0x7fffffffffffffffU, 0x8000000000000000U, ~0ULL, 12345U};
    for (int round = 0; round < 20000; ++round) {
        auto v = t;
        for (auto* field : {&v.instance_id,
                            &v.source_session_id,
                            &v.source_sequence,
                            &v.tx_sequence,
                            &v.reference_epoch,
                            &v.metadata_revision,
                            &v.source_received_ns,
                            &v.source_age_at_send_ns,
                            &v.sample_time_ms,
                            &v.sample_clock_epoch}) {
            if (random.below(4) == 0U) {
                *field = edges.at(random.below(edges.size()));
            }
        }
        v.sample_time_kind = static_cast<std::uint32_t>(random.below(5));
        if (random.below(16) == 0U) {
            v.protocol_version = static_cast<std::uint32_t>(random.next());
        }
        std::array<float, 4> values{};
        std::ranges::generate(values, [&random] {
            return random.below(8) == 0U ? std::bit_cast<float>(static_cast<std::uint32_t>(random.next()))
                                         : static_cast<float>(static_cast<double>(random.below(2000)) - 1000.0);
        });
        const bool quaternion = random.below(2) == 0U;
        compare(pose(quaternion, v, std::span{values.data(), quaternion ? 4U : 3U}));
    }
}

void mutate_datagrams(Random& random, const std::vector<Bytes>& seeds) {
    for (int round = 0; round < 100000; ++round) {
        auto packet = seeds.at(random.below(seeds.size()));
        const auto edits = 1U + random.below(4);
        for (std::size_t edit = 0; edit < edits; ++edit) {
            const auto at = random.below(packet.size() + 1U);
            switch (random.below(4)) {
            case 0:
                if (at < packet.size()) {
                    packet.at(at) = static_cast<std::byte>(random.next());
                }
                break;
            case 1:
                packet.resize(at);
                break;
            case 2:
                packet.insert(packet.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::byte>(random.next()));
                break;
            default:
                if (at < packet.size()) {
                    packet.at(at) = static_cast<std::byte>(packet.at(at) ^ std::byte{1});
                }
                break;
            }
        }
        compare(packet);
    }
}

// JSON-level mutations, re-framed so the JSON parsers actually see them.
void mutate_json(Random& random) {
    constexpr std::string_view alphabet = "{}[]\",:0123456789-+.eE \t\\/u tfnalsr\x7f\xc2\x85\xef\xbb\xbf\xed\xa0";
    const auto json_seeds = json_edge_cases();
    for (int round = 0; round < 100000; ++round) {
        auto json = json_seeds.at(random.below(json_seeds.size()));
        const auto edits = 1U + random.below(3);
        for (std::size_t edit = 0; edit < edits; ++edit) {
            const auto at = random.below(json.size() + 1U);
            const char c = alphabet.at(random.below(alphabet.size()));
            switch (random.below(3)) {
            case 0:
                if (at < json.size()) {
                    json.at(at) = c;
                }
                break;
            case 1:
                if (at < json.size()) {
                    json.erase(at, 1U);
                }
                break;
            default:
                json.insert(at, 1U, c);
                break;
            }
        }
        if (json.find('\0') == std::string::npos) {
            compare(telemetry(json, random.below(2) == 0U));
        }
    }
}

void differential_decode() {
    const auto seeds = seed_corpus();
    compare_seeds_and_prefixes(seeds);
    Random random(0x5eed0001U);
    sweep_pose_fields(random);
    mutate_datagrams(random, seeds);
    mutate_json(random);
}

void differential_source_ids() {
    for (const std::string_view value : {"",
                                         "head",
                                         " ",
                                         "   ",
                                         "a b",
                                         "\t",
                                         "\x7f",
                                         "\xc2\x80",
                                         "\xc2\x9f",
                                         "\xc2\xa0",
                                         "\xc2\xa0x",
                                         "\xc0\xaf",
                                         "\xe0\x80\xaf",
                                         "\xed\x9f\xbf",
                                         "\xed\xa0\x80",
                                         "\xef\xbf\xbf",
                                         "\xf4\x8f\xbf\xbf",
                                         "\xf4\x90\x80\x80",
                                         "\xf8\x88\x80\x80\x80",
                                         "\xe3\x80\x80",
                                         "\xe2\x80\x8a",
                                         "\xe2\x80\x8b",
                                         "\xe1\x9a\x80",
                                         "\xe2\x81\x9f",
                                         "\xc2",
                                         "\xe8\x80",
                                         "\xe8\x80\xb3\xe6\x9c\xba"}) {
        compare_source_id(value);
    }
    compare_source_id(std::string(256, 'x'));
    compare_source_id(std::string(257, 'x'));
    compare_source_id(std::string_view{"a\0b", 3});
    Random random(0x5eed0002U);
    for (int round = 0; round < 200000; ++round) {
        std::string value(random.below(8), '\0');
        for (auto& c : value) {
            const auto r = random.below(4);
            c = static_cast<char>(r == 0U ? 0x20U + random.below(0x60U) : 0x80U + random.below(0x80U));
        }
        compare_source_id(value);
    }
}

void differential_source_order() {
    Random random(0x5eed0003U);
    for (int stream = 0; stream < 2000; ++stream) {
        legacy::OscSourceOrder expected;
        current::OscSourceOrder actual;
        auto t = fixture();
        for (int step = 0; step < 64; ++step) {
            const auto small = [&random] { return static_cast<std::uint64_t>(random.below(6)); };
            switch (random.below(8)) {
            case 0:
                t.instance_id = 1U + small();
                break;
            case 1:
                t.source_session_id = 1U + small();
                break;
            case 2:
                t.sample_time_kind = static_cast<std::uint32_t>(random.below(3));
                t.sample_clock_epoch = small();
                break;
            default:
                break;
            }
            t.tx_sequence = random.below(5) == 0U ? small() : t.tx_sequence + small();
            t.source_sequence = random.below(5) == 0U ? small() : t.source_sequence + small();
            t.metadata_revision =
                random.below(6) == 0U ? small() : t.metadata_revision + (random.below(4) == 0U ? 1U : 0U);
            t.reference_epoch = random.below(6) == 0U ? small() : t.reference_epoch + (random.below(4) == 0U ? 1U : 0U);
            t.source_received_ns = random.below(5) == 0U ? small() : t.source_received_ns + small();
            t.sample_time_ms = random.below(5) == 0U ? small() : t.sample_time_ms + small();
            const bool a = expected.accept(t);
            const bool b = actual.accept(t);
            require(a == b, "source order acceptance differs");
            require(expected.last_gap() == actual.last_gap(), "source order gap differs");
            const auto probe = 1U + small();
            require(expected.retired(probe) == actual.retired(probe), "source order retirement differs");
        }
    }
}

} // namespace

int main() {
    try {
        differential_decode();
        differential_source_ids();
        differential_source_order();
        std::cout << "OSC protocol differential: " << counts().compared << " datagrams, " << counts().accepted
                  << " accepted\n";
        require(counts().accepted > 1000U, "corpus must exercise accepted messages");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
