#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/c_api.h"

#include "head_rotation.h"
#include "osc_head_tracking_protocol.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace {
using Timing = mradm::HeadTrackingTiming;
using Kind = mradm::realtime::HeadTrackingMessageKind;
void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string{message});
    }
}
bool approximately_equal(double a, double b, double tolerance = 0.0005) {
    return std::abs(a - b) < tolerance;
}
void text(std::vector<std::byte>& bytes, std::string_view value) {
    std::ranges::transform(value, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
    bytes.push_back(std::byte{0});
    while (bytes.size() % 4U != 0U) {
        bytes.push_back(std::byte{0});
    }
}
void integer(std::vector<std::byte>& bytes, std::uint64_t value, int width) {
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
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
void advance(Timing& t) {
    ++t.source_sequence;
    ++t.tx_sequence;
    t.source_received_ns += 10000000U;
    if (t.sample_time_kind != 0U) {
        t.sample_time_ms += 10;
    }
}
std::vector<std::byte> packet(std::string_view address,
                              std::span<const float> values,
                              const Timing& t = fixture(),
                              std::string_view source = "head") {
    std::vector<std::byte> bytes;
    text(bytes, address);
    text(bytes, std::string{",ishhhhhhhhihh"} + std::string(values.size(), 'f'));
    integer(bytes, t.protocol_version, 4);
    text(bytes, source);
    for (const auto v : {t.instance_id,
                         t.source_session_id,
                         t.source_sequence,
                         t.tx_sequence,
                         t.reference_epoch,
                         t.metadata_revision,
                         t.source_received_ns,
                         t.source_age_at_send_ns}) {
        integer(bytes, v, 8);
    }
    integer(bytes, t.sample_time_kind, 4);
    integer(bytes, t.sample_time_ms, 8);
    integer(bytes, t.sample_clock_epoch, 8);
    for (float v : values) {
        integer(bytes, std::bit_cast<std::uint32_t>(v), 4);
    }
    return bytes;
}
std::vector<std::byte> euler(const Timing& t = fixture(), float yaw = 30.0F, std::string_view source = "head") {
    return packet("/posebridge/euler", std::array{yaw, 20.0F, 10.0F}, t, source);
}
nlohmann::json metadata(const Timing& t,
                        std::uint64_t sequence,
                        bool info,
                        std::string_view source = "head",
                        std::string_view state = "active") {
    const auto decimal = [](auto v) { return std::to_string(v); };
    nlohmann::json j{{"schema", 3},
                     {"kind", info ? "info" : "status"},
                     {"source_id", source},
                     {"instance_id", decimal(t.instance_id)},
                     {"session_id", decimal(t.source_session_id)},
                     {"reference_epoch", decimal(t.reference_epoch)},
                     {"metadata_revision", decimal(t.metadata_revision)},
                     {"message_seq", decimal(sequence)}};
    if (info) {
        j["descriptor"] = {{"source_id", source},
                           {"instance_id", decimal(t.instance_id)},
                           {"session_id", decimal(t.source_session_id)},
                           {"reference_epoch", decimal(t.reference_epoch)},
                           {"metadata_revision", decimal(t.metadata_revision)},
                           {"coordinate_profile", "posebridge.yxz.v1"}};
    } else {
        j["status"] = {{"session_id", decimal(t.source_session_id)},
                       {"session_samples", decimal(t.source_sequence)},
                       {"state", state}};
    }
    return j;
}
std::vector<std::byte> telemetry(const nlohmann::json& j, bool info) {
    std::vector<std::byte> bytes;
    text(bytes, info ? "/posebridge/info" : "/posebridge/status");
    text(bytes, ",s");
    text(bytes, j.dump());
    return bytes;
}
mradm::realtime::HeadTrackingMessage decoded(const std::vector<std::byte>& bytes) {
    const auto result = mradm::realtime::decode_head_tracking_osc(bytes);
    if (!result) {
        throw std::runtime_error("valid message rejected");
    }
    return *result;
}
// One sequence of shared protocol vectors covers the wire contract.
// NOLINTNEXTLINE(readability-function-size)
void parser_contract() {
    struct Vector {
        std::array<float, 3> angles;
        std::array<float, 4> q;
    };
    const std::array vectors{Vector{{0, 0, 0}, {0, 0, 0, 1}},
                             Vector{{90, 0, 0}, {0, 0.707106781F, 0, 0.707106781F}},
                             Vector{{0, 30, 0}, {0.258819045F, 0, 0, 0.965925826F}},
                             Vector{{0, 0, 30}, {0, 0, 0.258819045F, 0.965925826F}},
                             Vector{{30, 20, 10}, {0.189307857F, 0.239298338F, 0.038134576F, 0.951548525F}},
                             Vector{{179, 0, 0}, {0, 0.999961923F, 0, 0.008726535F}},
                             Vector{{-179, 0, 0}, {0, -0.999961923F, 0, 0.008726535F}}};
    for (const auto& v : vectors) {
        const auto pose = decoded(packet("/posebridge/euler", v.angles)).orientation;
        for (std::size_t i = 0; i < 4U; ++i) {
            require(approximately_equal(pose.quaternion_xyzw.at(i), v.q.at(i), 1e-6), "quaternion vector");
        }
        for (float scale : {1.0F, -1.0F, 3.0F}) {
            auto q = v.q;
            std::ranges::transform(q, q.begin(), [scale](float component) { return component * scale; });
            const auto result = decoded(packet("/posebridge/quaternion", q)).orientation;
            for (std::size_t i = 0; i < 3U; ++i) {
                require(approximately_equal(result.euler_deg.at(i), v.angles.at(i)),
                        "quaternion sign and normalization");
            }
        }
    }
    mradm::ListenerOrientation listener;
    listener.yaw_deg = 90;
    const auto [az, el] = mradm::render_common::HeadRotation{listener}.rotate_az_el(0, 0);
    require(approximately_equal(az, -90) && approximately_equal(el, 0), "world-fixed source");
    auto timing = fixture();
    timing.instance_id = (1ULL << 55U) + 1U;
    timing.source_sequence = (1ULL << 54U) + 3U;
    timing.source_received_ns = (1ULL << 53U) + 5U;
    timing.sample_time_kind = 1;
    timing.sample_time_ms = 473398726930ULL;
    timing.sample_clock_epoch = 1;
    const auto good = euler(timing);
    const auto got = decoded(good).timing;
    require(got.instance_id == timing.instance_id && got.source_sequence == timing.source_sequence &&
                got.source_received_ns == timing.source_received_ns && got.sample_time_ms == timing.sample_time_ms,
            "int64 precision");
    for (std::size_t size = 0; size < good.size(); ++size) {
        require(!mradm::realtime::decode_head_tracking_osc(std::span{good.data(), size}), "truncated pose");
    }
    auto bad = good;
    bad.push_back(std::byte{0});
    require(!mradm::realtime::decode_head_tracking_osc(bad), "trailing byte");
    bad = good;
    bad[std::strlen("/posebridge/euler")] = std::byte{1};
    require(!mradm::realtime::decode_head_tracking_osc(bad), "padding");
    for (float value : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        require(!mradm::realtime::decode_head_tracking_osc(euler(timing, value)), "nonfinite");
    }
    require(!mradm::realtime::decode_head_tracking_osc(
                packet("/posebridge/quaternion", std::array{0.0F, 0.0F, 0.0F, 0.0F})),
            "zero quaternion");
    require(!mradm::realtime::decode_head_tracking_osc(euler(timing, 0, "")), "empty identity");
    require(!mradm::realtime::decode_head_tracking_osc(euler(timing, 0, std::string(257, 'x'))), "long identity");
    require(!mradm::realtime::valid_source_id(std::string{"\xc0\xaf", 2}), "invalid UTF-8");
    require(mradm::realtime::valid_source_id("耳机"), "UTF-8 source label");
    require(!mradm::realtime::valid_source_id("　"), "Unicode-only whitespace source rejected");
    for (const auto field : {&Timing::instance_id,
                             &Timing::source_session_id,
                             &Timing::source_sequence,
                             &Timing::tx_sequence,
                             &Timing::reference_epoch,
                             &Timing::metadata_revision}) {
        auto invalid = timing;
        invalid.*field = 0;
        require(!mradm::realtime::decode_head_tracking_osc(euler(invalid)), "zero key rejected");
        invalid.*field = std::numeric_limits<std::uint64_t>::max();
        require(!mradm::realtime::decode_head_tracking_osc(euler(invalid)), "negative int64 rejected");
    }
    auto invalid = timing;
    invalid.sample_time_kind = 0;
    require(!mradm::realtime::decode_head_tracking_osc(euler(invalid)), "absent clock with nonzero metadata");
    invalid = timing;
    invalid.source_age_at_send_ns = 500000000U;
    require(!mradm::realtime::decode_head_tracking_osc(euler(invalid)), "already expired at send");
    invalid = timing;
    invalid.protocol_version = 2;
    require(decoded(euler(invalid)).kind == Kind::incompatible, "version mismatch reported");
    require(decoded(packet("/posebridge/v2/euler", std::array{0.0F, 0.0F, 0.0F})).kind == Kind::incompatible,
            "old address rejected explicitly");
    for (bool info : {false, true}) {
        auto j = metadata(timing, 1, info);
        const auto bytes = telemetry(j, info);
        require(decoded(bytes).kind == (info ? Kind::info : Kind::status), "telemetry message");
        j["instance_id"] = timing.instance_id;
        require(!mradm::realtime::decode_head_tracking_osc(telemetry(j, info)), "JSON identity must be decimal string");
        j = metadata(timing, 1, info);
        j[info ? "descriptor" : "status"]["session_id"] = "999";
        require(!mradm::realtime::decode_head_tracking_osc(telemetry(j, info)), "nested identity mismatch");
        j = metadata(timing, 1, info);
        j["extra"] = std::string(8192, 'x');
        require(!mradm::realtime::decode_head_tracking_osc(telemetry(j, info)), "oversized JSON");
    }
}
void require_same_rotation(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    double same = 0.0;
    double opposite = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double difference = static_cast<double>(a.at(i)) - b.at(i);
        const double sum = static_cast<double>(a.at(i)) + b.at(i);
        same += difference * difference;
        opposite += sum * sum;
    }
    require(std::min(same, opposite) < 1e-11, "Euler output must preserve the input rotation, including at poles");
}
void orientation_poles() {
    const std::array<float, 4> quarter_turn{0.5F, 0.5F, -0.5F, 0.5F};
    const auto pole = decoded(packet("/posebridge/quaternion", quarter_turn)).orientation;
    require_same_rotation(quarter_turn,
                          decoded(packet("/posebridge/euler", pole.euler_deg)).orientation.quaternion_xyzw);
    for (float pitch : {-90.001F, -90.0F, -89.99999F, -89.9999F, 89.9999F, 89.99999F, 90.0F, 90.001F}) {
        for (float yaw : {-150.0F, 0.0F, 30.0F, 70.0F, 179.0F}) {
            for (float roll : {-75.0F, 0.0F, 10.0F, 60.0F}) {
                const auto source = decoded(packet("/posebridge/euler", std::array{yaw, pitch, roll})).orientation;
                const auto round_trip = decoded(packet("/posebridge/euler", source.euler_deg)).orientation;
                require_same_rotation(source.quaternion_xyzw, round_trip.quaternion_xyzw);
                for (float scale : {1.0F, -1.0F, 3.0F}) {
                    auto q = source.quaternion_xyzw;
                    std::ranges::transform(q, q.begin(), [scale](float value) { return value * scale; });
                    const auto result = decoded(packet("/posebridge/quaternion", q)).orientation;
                    const auto restored = decoded(packet("/posebridge/euler", result.euler_deg)).orientation;
                    require_same_rotation(source.quaternion_xyzw, restored.quaternion_xyzw);
                }
            }
        }
    }
}
void ordering_contract() {
    mradm::realtime::OscSourceOrder order;
    auto t = fixture();
    t.sample_time_kind = 1;
    t.sample_time_ms = 100;
    t.sample_clock_epoch = 1;
    require(order.accept(t) && !order.accept(t), "duplicate pose");
    advance(t);
    t.tx_sequence += 2;
    require(order.accept(t) && order.last_gap() == 2U, "tx gaps independent of sample gaps");
    auto bad = t;
    advance(bad);
    bad.source_received_ns = 0;
    require(!order.accept(bad), "host time reversal");
    bad = t;
    advance(bad);
    bad.sample_time_ms = t.sample_time_ms;
    require(!order.accept(bad), "repeated device timestamp");
    ++bad.sample_clock_epoch;
    bad.sample_time_ms = 0;
    require(order.accept(bad), "explicit clock reset");
    t = bad;
    auto absent = t;
    advance(absent);
    absent.sample_time_kind = 0;
    absent.sample_time_ms = 0;
    absent.sample_clock_epoch = 0;
    require(order.accept(absent), "absent timestamp");
    advance(bad);
    ++bad.tx_sequence;
    ++bad.source_sequence;
    bad.sample_clock_epoch = 1;
    require(!order.accept(bad), "absence does not erase clock history");
    t = absent;
    advance(t);
    ++t.source_session_id;
    t.source_sequence = 1;
    t.source_received_ns = 0;
    require(!order.accept(t), "new device session requires metadata/reference advance");
    ++t.metadata_revision;
    ++t.reference_epoch;
    require(order.accept(t), "reconnect");
    auto next = fixture();
    ++next.instance_id;
    require(order.accept(next) && !order.accept(t), "old instance retired");
}
using Receiver = std::unique_ptr<adm_osc_head_tracking_t, decltype(&adm_destroy_osc_head_tracking)>;
Receiver create(std::uint32_t port, const char* source = nullptr) {
    adm_osc_head_tracking_config_t cfg{sizeof(cfg), port, source};
    adm_osc_head_tracking_t* handle = nullptr;
    require(adm_create_osc_head_tracking(&cfg, &handle) == ADM_ERROR_OK && handle != nullptr, "create");
    return Receiver{handle, adm_destroy_osc_head_tracking};
}
adm_head_tracking_pose_t pose(const Receiver& receiver) {
    adm_head_tracking_pose_t p{};
    p.struct_size = sizeof(p);
    require(adm_osc_head_tracking_get_pose(receiver.get(), &p) == ADM_ERROR_OK, "pose getter");
    return p;
}
adm_osc_head_tracking_status_t status(const Receiver& receiver) {
    adm_osc_head_tracking_status_t s{};
    s.struct_size = sizeof(s);
    require(adm_osc_head_tracking_get_status(receiver.get(), &s) == ADM_ERROR_OK, "status getter");
    return s;
}
nlohmann::json snapshot(const Receiver& receiver) {
    char* value = nullptr;
    require(adm_osc_head_tracking_snapshot_json(receiver.get(), &value) == ADM_ERROR_OK && value != nullptr,
            "JSON ownership");
    const auto parsed = nlohmann::json::parse(value);
    adm_free_string(value);
    return parsed;
}
template <class Predicate> void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{4};
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "receiver timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}
class Sender {
  public:
    Sender() : socket_(open_socket()) {}
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;
    ~Sender() {
#ifdef _WIN32
        closesocket(socket_);
        WSACleanup();
#else
        ::close(socket_);
#endif
    }
    void send(std::uint32_t port, const std::vector<std::byte>& bytes) const {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(static_cast<std::uint16_t>(port));
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto sent = ::sendto(socket_,
                                   reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<int>(bytes.size()),
                                   0,
                                   reinterpret_cast<sockaddr*>(&target),
                                   sizeof(target));
        require(sent >= 0 && static_cast<std::size_t>(sent) == bytes.size(), "send datagram");
    }

  private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket k_invalid = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket k_invalid = -1;
#endif
    static Socket open_socket() {
#ifdef _WIN32
        WSADATA d{};
        require(WSAStartup(MAKEWORD(2, 2), &d) == 0, "WinSock startup");
#endif
        const auto value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(value != k_invalid, "sender socket");
        return value;
    }
    Socket socket_;
};
void deliver(const Sender& sender, const Receiver& receiver, const std::vector<std::byte>& bytes) {
    const auto before = status(receiver);
    sender.send(before.bound_port, bytes);
    wait_until([&] { return status(receiver).packets_received > before.packets_received; });
}
void telemetry_instance_ordering() {
    Sender sender;
    auto receiver = create(0, "head");
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "telemetry ordering start");
    auto t = fixture();
    deliver(sender, receiver, euler(t));
    auto stopped = t;
    stopped.source_sequence += 2;
    deliver(sender, receiver, telemetry(metadata(t, 10, true), true));
    deliver(sender, receiver, telemetry(metadata(stopped, 10, false, "head", "stopped"), false));
    require(pose(receiver).fresh == 0U, "confirmed stop invalidates pose");

    // More candidates than the bounded cache can hold must not evict the current instance.
    auto candidate = fixture();
    for (std::uint64_t i = 1; i <= 20; ++i) {
        candidate.instance_id = t.instance_id + i;
        candidate.source_session_id = 0;
        candidate.source_sequence = 0;
        deliver(sender, receiver, telemetry(metadata(candidate, 1, true), true));
        deliver(sender, receiver, telemetry(metadata(candidate, 1, false, "head", "connecting"), false));
    }
    auto all = snapshot(receiver);
    require(all["info"]["message_seq"] == "10" && all["source_status"]["message_seq"] == "10",
            "candidate telemetry cannot replace current metadata");
    const auto before = status(receiver);
    deliver(sender, receiver, telemetry(metadata(stopped, 9, false), false));
    deliver(sender, receiver, telemetry(metadata(t, 9, true), true));
    require(status(receiver).rejected_packets == before.rejected_packets + 2U && pose(receiver).fresh == 0U,
            "candidate telemetry cannot erase current replay protection");
    advance(t);
    deliver(sender, receiver, euler(t));
    require(pose(receiver).fresh == 0U, "cached stop still covers a delayed pose preceding the stop");
    advance(t);
    ++t.metadata_revision;
    deliver(sender, receiver, euler(t));
    all = snapshot(receiver);
    require(all["info_matches_pose"] == false && all["status_matches_pose"] == false && pose(receiver).fresh != 0U,
            "metadata association is recomputed after a pose revision changes");

    candidate.source_session_id = fixture().source_session_id;
    candidate.source_sequence = 1;
    candidate.metadata_revision = 2;
    deliver(sender, receiver, telemetry(metadata(candidate, 2, true), true));
    deliver(sender, receiver, telemetry(metadata(candidate, 2, false), false));
    require(pose(receiver).instance_id == t.instance_id, "telemetry cannot take over the pose binding");
    deliver(sender, receiver, euler(candidate));
    all = snapshot(receiver);
    require(pose(receiver).instance_id == candidate.instance_id && all["info_matches_pose"] == true &&
                all["status_matches_pose"] == true && all["source_status"]["message_seq"] == "2",
            "new pose selects its previously cached telemetry");
    const auto rejected = status(receiver).rejected_packets;
    deliver(sender, receiver, telemetry(metadata(t, 11, false), false));
    require(status(receiver).rejected_packets == rejected + 1U, "retired instance stays rejected");
}
void status_reference_changes() {
    Sender sender;
    for (bool with_info : {false, true}) {
        auto receiver = create(0, "head");
        require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "status reference start");
        auto t = fixture();
        deliver(sender, receiver, euler(t));
        if (with_info) {
            deliver(sender, receiver, telemetry(metadata(t, 1, true), true));
        }
        auto newer = t;
        ++newer.metadata_revision;
        deliver(sender, receiver, telemetry(metadata(newer, 1, false), false));
        require(pose(receiver).fresh != 0U, "metadata-only change keeps the same reference valid");
        ++newer.metadata_revision;
        ++newer.reference_epoch;
        deliver(sender, receiver, telemetry(metadata(newer, 2, false), false));
        require(pose(receiver).fresh == 0U && status(receiver).heartbeat_alive != 0U,
                "status reference change invalidates the pose independently of info");
        advance(t);
        deliver(sender, receiver, euler(t));
        require(pose(receiver).fresh == 0U, "later sample in the old reference cannot undo invalidation");
        advance(t);
        t.metadata_revision = newer.metadata_revision;
        t.reference_epoch = newer.reference_epoch;
        deliver(sender, receiver, euler(t));
        require(pose(receiver).fresh != 0U && snapshot(receiver)["status_matches_pose"] == true,
                "matching new-reference pose restores freshness");
        const auto rejected = status(receiver).rejected_packets;
        --newer.reference_epoch;
        deliver(sender, receiver, telemetry(metadata(newer, 3, false), false));
        require(status(receiver).rejected_packets == rejected + 1U && pose(receiver).fresh != 0U,
                "old reference cannot replace the recovered state");
    }
}
// This integration scenario deliberately retains one receiver across lifecycle transitions.
// NOLINTNEXTLINE(readability-function-size)
void receiver_contract() {
    Sender sender;
    auto receiver = create(0, "head");
    require(pose(receiver).has_pose == 0U && snapshot(receiver)["pose"].is_null(), "no manufactured first pose");
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "start");
    const auto initial = status(receiver);
    const auto port = initial.bound_port;
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK &&
                status(receiver).session_id == initial.session_id,
            "idempotent start");
    auto occupied = create(port);
    require(adm_osc_head_tracking_start(occupied.get()) == ADM_ERROR_IO, "exclusive port");
    auto t = fixture();
    sender.send(port, euler(t, 60, "other"));
    wait_until([&] { return status(receiver).ignored_sources == 1U; });
    require(pose(receiver).has_pose == 0U, "explicit source filter");
    sender.send(port, euler(t));
    wait_until([&] { return pose(receiver).receiver_sequence == 1U; });
    require(pose(receiver).fresh != 0U && approximately_equal(pose(receiver).yaw_deg, 30), "first pose");
    sender.send(port, telemetry(metadata(t, 1, true), true));
    sender.send(port, telemetry(metadata(t, 1, false), false));
    wait_until([&] { return status(receiver).telemetry_packets == 2U; });
    auto all = snapshot(receiver);
    require(all["info_matches_pose"] == true && all["status_matches_pose"] == true &&
                all["pose"]["instance_id"] == "123",
            "atomic matching metadata");
    std::uint64_t message_sequence = 2;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds{620};
    while (std::chrono::steady_clock::now() < end) {
        sender.send(port, telemetry(metadata(t, message_sequence++, false), false));
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
    }
    require(pose(receiver).fresh == 0U && status(receiver).heartbeat_alive != 0U &&
                pose(receiver).receiver_sequence == 1U,
            "heartbeat never refreshes orientation");
    advance(t);
    sender.send(port, euler(t));
    wait_until([&] { return pose(receiver).receiver_sequence == 2U; });
    require(pose(receiver).fresh != 0U && status(receiver).recovery_count == 1U, "new sample recovers");
    auto old = t;
    --old.source_sequence;
    sender.send(port, telemetry(metadata(old, message_sequence++, false, "head", "stopped"), false));
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    require(pose(receiver).fresh != 0U, "older stopped status cannot override newer sample");
    sender.send(port, telemetry(metadata(t, message_sequence++, false, "head", "stopped"), false));
    wait_until([&] { return pose(receiver).fresh == 0U; });
    advance(t);
    t.source_sequence += 6;
    ++t.tx_sequence;
    sender.send(port, euler(t));
    wait_until([&] { return pose(receiver).receiver_sequence == 3U; });
    require(status(receiver).missing_tx_packets == 1U, "sample coalescing is not UDP loss");
    const auto accepted = pose(receiver);
    const auto rejected = status(receiver).rejected_packets;
    sender.send(port, euler(t));
    wait_until([&] { return status(receiver).rejected_packets > rejected; });
    require(pose(receiver).receiver_received_ns == accepted.receiver_received_ns,
            "duplicate cannot refresh receive time");
    auto next = fixture();
    ++next.instance_id;
    sender.send(port, euler(next, 45));
    wait_until([&] { return pose(receiver).instance_id == next.instance_id; });
    const auto total = status(receiver).packets_received;
    sender.send(port, telemetry(metadata(t, message_sequence, false), false));
    sender.send(port, euler(t));
    wait_until([&] { return status(receiver).packets_received >= total + 2U; });
    require(pose(receiver).instance_id == next.instance_id, "late retired instance cannot replace pose or heartbeat");
    sender.send(port, packet("/posebridge/v1/euler", std::array{0.0F, 0.0F, 0.0F}));
    wait_until([&] { return status(receiver).protocol_mismatches == 1U; });
    require(std::strlen(adm_osc_head_tracking_last_error_message(receiver.get())) > 0U, "version mismatch diagnostic");
    sender.send(port, telemetry(metadata(next, 1, false), false));
    wait_until([&] { return status(receiver).has_heartbeat != 0U; });
    std::this_thread::sleep_for(std::chrono::milliseconds{3050});
    require(status(receiver).heartbeat_alive == 0U && pose(receiver).fresh == 0U, "independent heartbeat timeout");
    struct Future {
        adm_head_tracking_pose_t pose;
        std::uint64_t tail;
    } future{};
    future.pose.struct_size = sizeof(future);
    future.tail = 42;
    require(adm_osc_head_tracking_get_pose(receiver.get(), &future.pose) == ADM_ERROR_OK && future.tail == 42U,
            "unknown tail unchanged");
    future.pose.struct_size = 4;
    require(adm_osc_head_tracking_get_pose(receiver.get(), &future.pose) == ADM_ERROR_INVALID_ARGUMENT &&
                future.pose.struct_size == 4U,
            "short output unchanged");
    adm_osc_head_tracking_stop(receiver.get());
    adm_osc_head_tracking_stop(receiver.get());
    require(pose(receiver).fresh == 0U && status(receiver).heartbeat_alive == 0U, "stop clears activity");
    require(adm_osc_head_tracking_start(occupied.get()) == ADM_ERROR_OK, "port released");
    occupied.reset();
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK && pose(receiver).has_pose == 0U &&
                status(receiver).session_id != initial.session_id,
            "restart clears binding state");
    const auto final_port = status(receiver).bound_port;
    receiver.reset();
    auto reuse = create(final_port);
    require(adm_osc_head_tracking_start(reuse.get()) == ADM_ERROR_OK, "destroy releases port");
}
void automatic_binding() {
    Sender sender;
    auto receiver = create(0);
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "auto bind start");
    const auto port = status(receiver).bound_port;
    auto t = fixture();
    sender.send(port, telemetry(metadata(t, 1, true, "metadata-only"), true));
    sender.send(port, euler(t, 30, "chosen"));
    wait_until([&] { return pose(receiver).has_pose != 0U; });
    require(snapshot(receiver)["source_id"] == "chosen" && snapshot(receiver)["info_matches_pose"] == false,
            "metadata cannot claim first-pose binding");
    advance(t);
    sender.send(port, euler(t, 60, "other"));
    wait_until([&] { return status(receiver).ignored_sources == 1U; });
    require(approximately_equal(pose(receiver).yaw_deg, 30), "first logical source remains selected");
}
void invalid_arguments() {
    adm_osc_head_tracking_t* handle = nullptr;
    adm_osc_head_tracking_config_t config{sizeof(config), 65536U, nullptr};
    require(adm_create_osc_head_tracking(&config, &handle) == ADM_ERROR_INVALID_ARGUMENT, "invalid port");
    config.listen_port = 0;
    config.source_id = "\n";
    require(adm_create_osc_head_tracking(&config, &handle) == ADM_ERROR_INVALID_ARGUMENT, "invalid filter");
    require(adm_create_osc_head_tracking(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT, "null out");
    char untouched = 'x';
    char* text_value = &untouched;
    require(adm_osc_head_tracking_snapshot_json(nullptr, &text_value) == ADM_ERROR_INVALID_ARGUMENT &&
                text_value == nullptr,
            "JSON failure clears output");
    require(adm_osc_head_tracking_get_pose(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT, "null pose");
    adm_osc_head_tracking_stop(nullptr);
    adm_destroy_osc_head_tracking(nullptr);
}
} // namespace
int main() {
    try {
        parser_contract();
        orientation_poles();
        ordering_contract();
        invalid_arguments();
        telemetry_instance_ordering();
        status_reference_changes();
        receiver_contract();
        automatic_binding();
        std::cout << "PoseBridge current protocol, identity, clocks, telemetry and C ABI PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
