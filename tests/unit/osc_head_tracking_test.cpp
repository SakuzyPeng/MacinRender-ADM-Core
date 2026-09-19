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

void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string{message});
    }
}

bool approximately_equal(double first, double second, double tolerance = 0.0005) {
    return std::abs(first - second) < tolerance;
}

std::vector<std::byte>
packet(std::string_view address, std::span<const float> values, const mradm::HeadTrackingTiming* timing = nullptr) {
    std::vector<std::byte> bytes;
    const auto text = [&bytes](std::string_view value) {
        std::ranges::transform(
            value, std::back_inserter(bytes), [](char byte) { return static_cast<std::byte>(byte); });
        bytes.push_back(std::byte{0});
        while (bytes.size() % 4U != 0U) {
            bytes.push_back(std::byte{0});
        }
    };
    text(address);
    text(std::string(timing != nullptr ? ",hhhhih" : ",") + std::string(values.size(), 'f'));
    if (timing != nullptr) {
        const auto integer = [&bytes](std::uint64_t value, int width) {
            for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
                bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
            }
        };
        integer(timing->source_session_id, 8);
        integer(timing->source_sequence, 8);
        integer(timing->source_received_ns, 8);
        integer(timing->sample_time_ms, 8);
        integer(timing->sample_time_kind, 4);
        integer(timing->sample_clock_epoch, 8);
    }
    for (const auto value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (const int shift : {24, 16, 8, 0}) {
            bytes.push_back(static_cast<std::byte>((bits >> shift) & 255U));
        }
    }
    return bytes;
}

std::vector<std::byte> euler(float yaw, float pitch = 0.0F, float roll = 0.0F) {
    return packet("/posebridge/v1/euler", std::array{yaw, pitch, roll});
}

mradm::HeadTrackingOrientation decode_valid(std::span<const std::byte> bytes) {
    const auto decoded = mradm::realtime::decode_head_tracking_osc(bytes);
    if (!decoded) {
        throw std::runtime_error("valid protocol vector was rejected");
    }
    return decoded->orientation;
}

void parser_contract() {
    struct Vector {
        std::array<float, 3> angles;
        std::array<float, 4> quaternion;
    };
    const std::array vectors{Vector{{0, 0, 0}, {0, 0, 0, 1}},
                             Vector{{90, 0, 0}, {0, 0.707106781F, 0, 0.707106781F}},
                             Vector{{0, 30, 0}, {0.258819045F, 0, 0, 0.965925826F}},
                             Vector{{0, 0, 30}, {0, 0, 0.258819045F, 0.965925826F}},
                             Vector{{30, 20, 10}, {0.189307857F, 0.239298338F, 0.038134576F, 0.951548525F}},
                             Vector{{179, 0, 0}, {0, 0.999961923F, 0, 0.008726535F}},
                             Vector{{-179, 0, 0}, {0, -0.999961923F, 0, 0.008726535F}}};
    for (const auto& item : vectors) {
        const auto from_euler = decode_valid(packet("/posebridge/v1/euler", item.angles));
        for (std::size_t i = 0; i < 4U; ++i) {
            require(approximately_equal(from_euler.quaternion_xyzw.at(i), item.quaternion.at(i), 1.0e-6),
                    "YXZ quaternion test vector");
        }
        for (const float scale : {1.0F, -1.0F, 3.0F}) {
            auto scaled = item.quaternion;
            std::ranges::transform(scaled, scaled.begin(), [scale](float component) { return component * scale; });
            const auto pose = decode_valid(packet("/posebridge/v1/quaternion", scaled));
            for (std::size_t i = 0; i < 3U; ++i) {
                require(approximately_equal(pose.euler_deg.at(i), item.angles.at(i)),
                        "Euler/quaternion conventions agree, including -q");
            }
        }
    }
    const auto left = decode_valid(euler(90));
    mradm::ListenerOrientation orientation;
    orientation.yaw_deg = left.euler_deg[0];
    orientation.pitch_deg = left.euler_deg[1];
    orientation.roll_deg = left.euler_deg[2];
    const auto [azimuth, elevation] = mradm::render_common::HeadRotation{orientation}.rotate_az_el(0.0F, 0.0F);
    require(approximately_equal(azimuth, -90.0) && approximately_equal(elevation, 0.0),
            "world-fixed front source moves right on left head turn");
    const auto before = decode_valid(euler(179));
    const auto after = decode_valid(euler(-179));
    double dot = 0.0;
    for (std::size_t i = 0; i < 4U; ++i) {
        dot += static_cast<double>(before.quaternion_xyzw.at(i)) * after.quaternion_xyzw.at(i);
    }
    require(std::abs(dot) > 0.9998, "+/-180 crossings remain adjacent rotations for short-arc interpolation");

    const auto good = euler(30, 20, 10);
    for (std::size_t size = 0; size < good.size(); ++size) {
        require(!mradm::realtime::decode_head_tracking_osc(std::span{good.data(), size}),
                "every truncated prefix rejected");
    }
    auto malformed = good;
    malformed.push_back(std::byte{0});
    require(!mradm::realtime::decode_head_tracking_osc(malformed), "trailing bytes rejected");
    malformed = good;
    malformed[std::strlen("/posebridge/v1/euler")] = std::byte{1};
    require(!mradm::realtime::decode_head_tracking_osc(malformed), "nonzero string padding rejected");
    malformed = good;
    malformed[24] = std::byte{'i'}; // type tag belongs to the string, not a numeric payload
    require(!mradm::realtime::decode_head_tracking_osc(malformed), "wrong type tags rejected");
    require(!mradm::realtime::decode_head_tracking_osc(packet("/other", std::array{1.0F, 2.0F, 3.0F})),
            "unknown address rejected");
    require(!mradm::realtime::decode_head_tracking_osc(packet("#bundle", std::array{1.0F, 2.0F, 3.0F})),
            "bundles rejected");
    require(
        !mradm::realtime::decode_head_tracking_osc(packet("/posebridge/v1/quaternion", std::array{1.0F, 2.0F, 3.0F})),
        "wrong arity rejected");
    for (const float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        require(!mradm::realtime::decode_head_tracking_osc(euler(value)), "nonfinite Euler rejected");
        require(!mradm::realtime::decode_head_tracking_osc(
                    packet("/posebridge/v1/quaternion", std::array{value, 0.0F, 0.0F, 1.0F})),
                "nonfinite quaternion rejected");
    }
    for (const float value : {0.0F, 1.0e-8F}) {
        require(!mradm::realtime::decode_head_tracking_osc(
                    packet("/posebridge/v1/quaternion", std::array{value, 0.0F, 0.0F, 0.0F})),
                "degenerate quaternion rejected");
    }
    const float huge = std::numeric_limits<float>::max();
    const auto huge_quaternion = mradm::realtime::decode_head_tracking_osc(
        packet("/posebridge/v1/quaternion", std::array{huge, huge, huge, huge}));
    require(huge_quaternion && approximately_equal(huge_quaternion->orientation.quaternion_xyzw[0], 0.5),
            "double norm calculation avoids float overflow");
    require(mradm::realtime::decode_head_tracking_osc(euler(huge)).has_value(), "finite large Euler range reduction");
}

mradm::HeadTrackingTiming timing_fixture() {
    return {2U, 1U, 0x123456789abcdefULL, (1ULL << 53U) + 1U, (1ULL << 54U) + 3U, 473398726930ULL, 1U};
}

std::vector<std::byte> timed_euler(const mradm::HeadTrackingTiming& timing, float yaw = 30.0F) {
    return packet("/posebridge/v2/euler", std::array{yaw, 20.0F, 10.0F}, &timing);
}

void v2_parser_and_order_contract() {
    auto time = timing_fixture();
    for (const auto& bytes :
         {timed_euler(time), packet("/posebridge/v2/quaternion", std::array{0.0F, 0.0F, 0.0F, 1.0F}, &time)}) {
        const auto decoded = mradm::realtime::decode_head_tracking_osc(bytes);
        require(decoded && decoded->timing.protocol_version == 2U &&
                    decoded->timing.source_session_id == time.source_session_id &&
                    decoded->timing.source_sequence == time.source_sequence &&
                    decoded->timing.source_received_ns == time.source_received_ns &&
                    decoded->timing.sample_time_ms == time.sample_time_ms && decoded->timing.sample_time_kind == 1U &&
                    decoded->timing.sample_clock_epoch == 1U,
                "v2 metadata preserves int64 precision");
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            require(!mradm::realtime::decode_head_tracking_osc(std::span{bytes.data(), size}), "truncated v2 rejected");
        }
        auto trailing = bytes;
        trailing.push_back(std::byte{0});
        require(!mradm::realtime::decode_head_tracking_osc(trailing), "v2 extra fields rejected");
    }
    for (const auto field : {&mradm::HeadTrackingTiming::source_session_id,
                             &mradm::HeadTrackingTiming::source_sequence,
                             &mradm::HeadTrackingTiming::source_received_ns,
                             &mradm::HeadTrackingTiming::sample_time_ms,
                             &mradm::HeadTrackingTiming::sample_clock_epoch}) {
        auto negative = time;
        negative.*field = std::numeric_limits<std::uint64_t>::max();
        require(!mradm::realtime::decode_head_tracking_osc(timed_euler(negative)), "negative OSC integer rejected");
    }
    for (const auto field : {&mradm::HeadTrackingTiming::source_session_id,
                             &mradm::HeadTrackingTiming::source_sequence,
                             &mradm::HeadTrackingTiming::sample_clock_epoch}) {
        auto zero = time;
        zero.*field = 0;
        require(!mradm::realtime::decode_head_tracking_osc(timed_euler(zero)),
                "required positive metadata rejected at zero");
    }
    time.sample_time_kind = 3;
    require(!mradm::realtime::decode_head_tracking_osc(timed_euler(time)), "unknown clock kind rejected");
    time.sample_time_kind = 0;
    require(!mradm::realtime::decode_head_tracking_osc(timed_euler(time)), "absent time cannot have metadata");
    time.sample_time_ms = 0;
    time.sample_clock_epoch = 0;
    require(mradm::realtime::decode_head_tracking_osc(timed_euler(time)).has_value(), "absent time is explicit");
    time.sample_time_kind = 2;
    time.sample_clock_epoch = 1;
    require(mradm::realtime::decode_head_tracking_osc(timed_euler(time)).has_value(), "simulator time zero valid");
    require(!mradm::realtime::decode_head_tracking_osc(packet("/posebridge/v2/euler", std::array{0.0F, 0.0F, 0.0F})),
            "v2 cannot use v1 tags");
    require(
        !mradm::realtime::decode_head_tracking_osc(packet("/posebridge/v1/euler", std::array{0.0F, 0.0F, 0.0F}, &time)),
        "v1 cannot use v2 tags");
    require(!mradm::realtime::decode_head_tracking_osc(timed_euler(time, std::numeric_limits<float>::infinity())),
            "nonfinite v2 pose rejected");

    mradm::realtime::OscSourceOrder order;
    time = timing_fixture();
    require(order.accept(time) && !order.accept(time), "first source accepted, replay rejected");
    ++time.source_sequence;
    require(!order.accept(time), "new sequence cannot disguise duplicate device time");
    ++time.sample_time_ms;
    require(order.accept(time), "batch may share a source receive time");
    auto backward = time;
    ++backward.source_sequence;
    --backward.source_received_ns;
    ++backward.sample_time_ms;
    require(!order.accept(backward), "decreasing host time rejected");
    backward = time;
    ++backward.source_sequence;
    --backward.sample_time_ms;
    require(!order.accept(backward), "device time regression requires a new epoch");
    ++backward.sample_clock_epoch;
    require(order.accept(backward), "explicit clock reset accepted");
    auto absent = backward;
    ++absent.source_sequence;
    absent.sample_time_kind = 0;
    absent.sample_time_ms = 0;
    absent.sample_clock_epoch = 0;
    require(order.accept(absent), "absent timestamp is supported");
    time.source_sequence = absent.source_sequence + 1U;
    require(!order.accept(time), "absent packet cannot erase clock epoch history");
    time = backward;
    time.source_sequence = absent.source_sequence + 1U;
    time.sample_time_kind = 2;
    ++time.sample_time_ms;
    require(!order.accept(time), "clock kind change requires new epoch");
    ++time.sample_clock_epoch;
    require(order.accept(time), "new epoch permits kind change");
    auto new_source = timing_fixture();
    new_source.source_session_id = 42;
    new_source.source_sequence = 1;
    new_source.source_received_ns = 0;
    require(order.accept(new_source) && !order.accept(time), "retired session packets rejected");
    mradm::HeadTrackingTiming v1;
    v1.protocol_version = 1;
    require(order.accept(v1) && !order.accept(new_source), "v1 cannot clear v2 ordering memory");
}

using Receiver = std::unique_ptr<adm_osc_head_tracking_t, decltype(&adm_destroy_osc_head_tracking)>;

Receiver create(std::uint32_t port) {
    adm_osc_head_tracking_config_t config{sizeof(config), port};
    adm_osc_head_tracking_t* handle = nullptr;
    require(adm_create_osc_head_tracking(&config, &handle) == ADM_ERROR_OK && handle != nullptr, "create receiver");
    return Receiver{handle, adm_destroy_osc_head_tracking};
}

adm_osc_head_tracking_status_t status(const Receiver& receiver) {
    adm_osc_head_tracking_status_t result{};
    result.struct_size = sizeof(result);
    require(adm_osc_head_tracking_get_status(receiver.get(), &result) == ADM_ERROR_OK, "status query");
    return result;
}

adm_head_tracking_pose_t pose(const Receiver& receiver) {
    adm_head_tracking_pose_t result{};
    result.struct_size = sizeof(result);
    require(adm_osc_head_tracking_get_pose(receiver.get(), &result) == ADM_ERROR_OK, "pose query");
    return result;
}

adm_head_tracking_pose_v2_t pose_v2(const Receiver& receiver) {
    adm_head_tracking_pose_v2_t result{};
    result.struct_size = sizeof(result);
    require(adm_osc_head_tracking_get_pose_v2(receiver.get(), &result) == ADM_ERROR_OK, "v2 pose query");
    return result;
}

template <class Predicate> void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, "timed out waiting for receiver");
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
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(static_cast<std::uint16_t>(port));
#ifdef _WIN32
        const auto size = static_cast<int>(bytes.size());
#else
        const auto size = bytes.size();
#endif
        const auto sent = ::sendto(socket_,
                                   reinterpret_cast<const char*>(bytes.data()),
                                   size,
                                   0,
                                   reinterpret_cast<const sockaddr*>(&target),
                                   sizeof(target));
        require(sent >= 0 && static_cast<std::size_t>(sent) == bytes.size(), "send complete test datagram");
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
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "test WinSock startup");
#endif
        const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == k_invalid) {
#ifdef _WIN32
            WSACleanup();
#endif
            throw std::runtime_error("test sender socket");
        }
        return socket;
    }
    Socket socket_;
};

void receiver_contract() {
    Sender sender;
    auto receiver = create(0);
    require(status(receiver).state == ADM_OSC_HEAD_TRACKING_IDLE, "initial idle state");
    require((pose(receiver).has_pose == 0U) && (pose(receiver).fresh == 0U), "no synthetic pose before first packet");
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "bind ephemeral loopback port");
    const auto first_status = status(receiver);
    require(first_status.state == ADM_OSC_HEAD_TRACKING_WAITING && first_status.bound_port != 0U,
            "waiting without data");
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK &&
                status(receiver).session_id == first_status.session_id,
            "start is idempotent");
    const auto port = first_status.bound_port;
    auto occupied = create(port);
    require(adm_osc_head_tracking_start(occupied.get()) == ADM_ERROR_IO, "occupied port reports failure");
    require(status(occupied).state == ADM_OSC_HEAD_TRACKING_FAILED &&
                std::strlen(adm_osc_head_tracking_last_error_message(occupied.get())) > 0U,
            "bind failure diagnostics");

    sender.send(port, euler(30, 20, 10));
    wait_until([&] { return pose(receiver).sequence == 1U; });
    const auto first = pose(receiver);
    require((first.fresh != 0U) && (first.has_pose != 0U) && approximately_equal(first.yaw_deg, 30.0) &&
                approximately_equal(first.pitch_deg, 20.0) && approximately_equal(first.roll_deg, 10.0),
            "complete first pose");
    require(pose(receiver).sequence == first.sequence && pose(receiver).received_ns == first.received_ns,
            "polls do not manufacture samples");

    // Invalid input continues arriving across the 500 ms freshness boundary.
    const auto expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds{600};
    while (std::chrono::steady_clock::now() < expiry) {
        sender.send(port, euler(std::numeric_limits<float>::quiet_NaN()));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    require(status(receiver).state == ADM_OSC_HEAD_TRACKING_STALE, "invalid packets cannot keep input alive");
    const auto stale = pose(receiver);
    require((stale.has_pose != 0U) && (stale.fresh == 0U) && stale.sequence == first.sequence &&
                stale.received_ns == first.received_ns && approximately_equal(stale.yaw_deg, first.yaw_deg),
            "stale snapshot freezes last valid orientation");

    auto oversized = euler(45);
    oversized.resize(4096, std::byte{0});
    const auto rejected = status(receiver).rejected_packets;
    sender.send(port, oversized);
    wait_until([&] { return status(receiver).rejected_packets > rejected; });
    require((pose(receiver).fresh == 0U), "oversized valid prefix cannot revive the source");
    sender.send(port, euler(30, 20, 10));
    wait_until([&] { return pose(receiver).sequence == first.sequence + 1U; });
    require((pose(receiver).fresh != 0U) && status(receiver).recovery_count == 1U,
            "equal-valued new sample resumes and records recovery");

    // Leave the consumer idle while a complete burst arrives. It sees the final value,
    // not a queue that must be replayed one pose per poll.
    for (int index = 0; index < 32; ++index) {
        sender.send(port, euler(static_cast<float>(index)));
    }
    wait_until([&] { return approximately_equal(pose(receiver).yaw_deg, 31.0); });
    const auto latest = pose(receiver);
    require(latest.sequence > first.sequence + 2U, "receiver continues collecting while consumer is idle");

    struct ForwardPose {
        adm_head_tracking_pose_t pose;
        std::uint64_t future_field;
    } extended{};
    extended.pose.struct_size = sizeof(extended);
    extended.future_field = 0x123456789abcdef0ULL;
    require(adm_osc_head_tracking_get_pose(receiver.get(), &extended.pose) == ADM_ERROR_OK &&
                extended.future_field == 0x123456789abcdef0ULL,
            "unknown caller tail preserved");
    auto too_short = latest;
    too_short.struct_size = 4;
    require(adm_osc_head_tracking_get_pose(receiver.get(), &too_short) == ADM_ERROR_INVALID_ARGUMENT &&
                too_short.sequence == latest.sequence,
            "undersized output not partially written");

    adm_osc_head_tracking_stop(receiver.get());
    adm_osc_head_tracking_stop(receiver.get());
    require(status(receiver).state == ADM_OSC_HEAD_TRACKING_STOPPED && (pose(receiver).fresh == 0U) &&
                approximately_equal(pose(receiver).yaw_deg, 31.0),
            "stop preserves last pose and clears activity");
    require(adm_osc_head_tracking_start(occupied.get()) == ADM_ERROR_OK, "stop releases port, failed handle can retry");
    require(std::strlen(adm_osc_head_tracking_last_error_message(occupied.get())) == 0U,
            "successful restart clears fatal diagnostic");
    occupied.reset();
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "restart stopped receiver");
    require((pose(receiver).has_pose == 0U) && pose(receiver).sequence == 0U &&
                status(receiver).session_id != first_status.session_id,
            "restart creates empty new session");
    const auto final_port = status(receiver).bound_port;
    receiver.reset(); // destroy while listening must join and release the socket
    auto reused = create(final_port);
    require(adm_osc_head_tracking_start(reused.get()) == ADM_ERROR_OK, "destruction releases bound port");
}

void v2_receiver_contract() {
    Sender sender;
    auto receiver = create(0);
    require(pose_v2(receiver).protocol_version == 0U && pose_v2(receiver).pose.has_pose == 0U, "v2 no-data output");
    require(adm_osc_head_tracking_start(receiver.get()) == ADM_ERROR_OK, "v2 bind");
    const auto port = status(receiver).bound_port;
    auto time = timing_fixture();
    sender.send(port, timed_euler(time));
    wait_until([&] { return pose_v2(receiver).pose.sequence == 1U; });
    const auto first = pose_v2(receiver);
    require(first.protocol_version == 2U && first.source_sequence == time.source_sequence &&
                first.source_received_ns == time.source_received_ns && first.sample_time_ms == time.sample_time_ms &&
                first.pose.struct_size == sizeof(adm_head_tracking_pose_t) &&
                approximately_equal(first.pose.yaw_deg, 30.0),
            "atomic timing and orientation");
    require(pose(receiver).received_ns == first.pose.received_ns && pose(receiver).sequence == first.pose.sequence,
            "old getter preserves local clock and sequence semantics");
    const auto expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds{600};
    while (std::chrono::steady_clock::now() < expiry) {
        sender.send(port, timed_euler(time));
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
    }
    require(pose_v2(receiver).pose.fresh == 0U && pose_v2(receiver).pose.sequence == 1U &&
                pose_v2(receiver).pose.received_ns == first.pose.received_ns,
            "replayed v2 cannot keep input active");
    ++time.source_sequence;
    time.sample_time_ms = 5;
    ++time.sample_clock_epoch;
    sender.send(port, timed_euler(time));
    wait_until([&] { return pose_v2(receiver).pose.sequence == 2U; });
    require(pose_v2(receiver).sample_clock_epoch == 2U && status(receiver).recovery_count == 1U,
            "clock reset resumes freshness");
    struct FuturePose {
        adm_head_tracking_pose_v2_t value;
        std::uint64_t tail;
    } extended{};
    extended.value.struct_size = sizeof(extended);
    extended.tail = 0x123456789abcdef0ULL;
    require(adm_osc_head_tracking_get_pose_v2(receiver.get(), &extended.value) == ADM_ERROR_OK &&
                extended.tail == 0x123456789abcdef0ULL,
            "v2 unknown caller tail preserved");
    extended.value.struct_size = 4;
    require(adm_osc_head_tracking_get_pose_v2(receiver.get(), &extended.value) == ADM_ERROR_INVALID_ARGUMENT &&
                extended.value.struct_size == 4U,
            "undersized v2 output untouched");
    sender.send(port, euler(45));
    wait_until([&] { return pose_v2(receiver).pose.sequence == 3U; });
    const auto legacy = pose_v2(receiver);
    require(legacy.protocol_version == 1U && legacy.sample_time_kind == 0U && legacy.source_session_id == 0U &&
                legacy.source_sequence == 0U && legacy.source_received_ns == 0U && legacy.sample_time_ms == 0U &&
                legacy.sample_clock_epoch == 0U,
            "v1 clears exposed metadata without inventing sample time");
    const auto total = status(receiver).packets_received;
    sender.send(port, timed_euler(time));
    wait_until([&] { return status(receiver).packets_received > total; });
    require(pose_v2(receiver).pose.sequence == 3U, "v2 replay rejected even after v1");
    adm_osc_head_tracking_stop(receiver.get());
    require(pose_v2(receiver).pose.fresh == 0U, "v2 stop clears freshness");
}

void invalid_abi_arguments() {
    adm_osc_head_tracking_t* handle = nullptr;
    adm_osc_head_tracking_config_t config{sizeof(config), 65536U};
    require(adm_create_osc_head_tracking(&config, &handle) == ADM_ERROR_INVALID_ARGUMENT && handle == nullptr,
            "invalid port rejected");
    config.struct_size = 4;
    require(adm_create_osc_head_tracking(&config, &handle) == ADM_ERROR_INVALID_ARGUMENT, "undersized config rejected");
    require(adm_create_osc_head_tracking(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
            "null handle output rejected");
    require(adm_create_osc_head_tracking(nullptr, &handle) == ADM_ERROR_OK && handle != nullptr,
            "null config accepts default without binding");
    adm_destroy_osc_head_tracking(handle);
    require(adm_osc_head_tracking_start(nullptr) == ADM_ERROR_INVALID_ARGUMENT, "null start rejected");
    require(adm_osc_head_tracking_get_pose(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT, "null pose query rejected");
    require(adm_osc_head_tracking_get_pose_v2(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
            "null v2 pose query rejected");
    require(adm_osc_head_tracking_get_status(nullptr, nullptr) == ADM_ERROR_INVALID_ARGUMENT,
            "null status query rejected");
    adm_osc_head_tracking_stop(nullptr);
    adm_destroy_osc_head_tracking(nullptr);
}

} // namespace

int main() {
    try {
        parser_contract();
        v2_parser_and_order_contract();
        invalid_abi_arguments();
        receiver_contract();
        v2_receiver_contract();
        std::cout << "OSC head tracking: protocol, coordinates, C ABI, UDP freshness and lifecycle PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
