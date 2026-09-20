#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adm/head_tracking.h"

#include "osc_head_tracking_protocol.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace mradm {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto k_stale_after = std::chrono::milliseconds{500};
std::uint64_t allocate_session_id() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

#ifdef _WIN32
using Socket = SOCKET;
using AddressSize = int;
constexpr Socket k_invalid_socket = INVALID_SOCKET;
int socket_error() noexcept {
    return WSAGetLastError();
}
bool retryable(int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}
void close_socket(Socket value) noexcept {
    closesocket(value);
}
#else
using Socket = int;
using AddressSize = socklen_t;
constexpr Socket k_invalid_socket = -1;
int socket_error() noexcept {
    return errno;
}
bool retryable(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}
void close_socket(Socket value) noexcept {
    ::close(value);
}
#endif

} // namespace

struct OscHeadTrackingReceiver::Impl {
    explicit Impl(std::uint16_t port, std::string source) : requested_port(port), requested_source(std::move(source)) {}

  private:
    friend class OscHeadTrackingReceiver;
    std::uint16_t requested_port;
    std::string requested_source;
    Socket socket{k_invalid_socket};
#ifdef _WIN32
    bool winsock_started{false};
#endif
    std::jthread worker;
    mutable std::mutex mutex;
    OscHeadTrackingSnapshot state;
    realtime::OscSourceOrder source_order;
    Clock::time_point started_at;
    Clock::time_point received_at;
    Clock::time_point heartbeat_at;
    bool source_allows_pose{true};
    struct CachedTelemetry {
        realtime::HeadTrackingMessage message;
        Clock::time_point received_at;
    };
    // One latest message per kind and source instance, bounded to 17 instances.
    // Candidates never evict metadata belonging to the current pose instance.
    using TelemetryCache = std::vector<CachedTelemetry>;
    TelemetryCache info_cache;
    TelemetryCache status_cache;
    std::string error_message;

  public:
    void close() noexcept {
        if (socket != k_invalid_socket) {
            close_socket(socket);
            socket = k_invalid_socket;
        }
#ifdef _WIN32
        if (winsock_started) {
            WSACleanup();
            winsock_started = false;
        }
#endif
    }

    Result<void> fail(std::string message, int code, ErrorCode error_code = ErrorCode::io_error) {
        const std::lock_guard lock(mutex);
        state.state = OscHeadTrackingState::failed;
        if (socket == k_invalid_socket) {
            state.bound_port = 0;
        }
        error_message = std::move(message) + " (" + std::to_string(code) + ")";
        return make_error(error_code, error_message);
    }

    bool matches_current_instance(const CachedTelemetry& cached) const {
        return state.has_pose && cached.message.source_id == state.source_id &&
               cached.message.timing.instance_id == state.timing.instance_id;
    }

    const CachedTelemetry* current_telemetry(const TelemetryCache& cache) const {
        if (cache.empty()) {
            return nullptr;
        }
        if (!state.has_pose) {
            return &cache.back();
        }
        const auto entry =
            std::ranges::find_if(cache, [this](const auto& value) { return matches_current_instance(value); });
        return entry == cache.end() ? nullptr : &*entry;
    }

    void update_metadata() {
        const auto* info = current_telemetry(info_cache);
        const auto* status = current_telemetry(status_cache);
        const auto matches_pose = [this](const realtime::HeadTrackingMessage& message) {
            return state.has_pose && message.timing.instance_id == state.timing.instance_id &&
                   message.timing.source_session_id == state.timing.source_session_id &&
                   message.timing.metadata_revision == state.timing.metadata_revision &&
                   message.timing.reference_epoch == state.timing.reference_epoch;
        };
        const auto changes_reference = [this](const realtime::HeadTrackingMessage& message) {
            return state.has_pose && message.timing.metadata_revision >= state.timing.metadata_revision &&
                   (message.timing.reference_epoch > state.timing.reference_epoch ||
                    message.timing.source_session_id != state.timing.source_session_id);
        };
        state.info_json = info != nullptr ? info->message.json : "";
        state.status_json = status != nullptr ? status->message.json : "";
        state.info_matches_pose = info != nullptr && matches_pose(info->message);
        state.status_matches_pose = status != nullptr && matches_pose(status->message);
        state.has_heartbeat = status != nullptr;
        bool changed_reference = false;
        if (info != nullptr) {
            const auto& message = info->message;
            changed_reference = changes_reference(message);
        }
        if (status != nullptr) {
            const auto& message = status->message;
            changed_reference = changed_reference || changes_reference(message);
            heartbeat_at = status->received_at;
            if (state.has_pose && message.timing.metadata_revision >= state.timing.metadata_revision) {
                if (message.timing.source_session_id == state.timing.source_session_id) {
                    if (message.reported_samples >= state.timing.source_sequence) {
                        source_allows_pose = message.source_active;
                    }
                } else {
                    source_allows_pose = false;
                }
            }
        }
        if (changed_reference) {
            source_allows_pose = false;
        }
    }

    void accept_telemetry(const realtime::HeadTrackingMessage& message, Clock::time_point now) {
        auto& cache = message.kind == realtime::HeadTrackingMessageKind::info ? info_cache : status_cache;
        const auto& t = message.timing;
        const auto cached = std::ranges::find_if(cache, [&](const auto& value) {
            return value.message.source_id == message.source_id && value.message.timing.instance_id == t.instance_id;
        });
        const bool old = source_order.retired(t.instance_id) ||
                         (cached != cache.end() && (message.message_sequence <= cached->message.message_sequence ||
                                                    t.metadata_revision < cached->message.timing.metadata_revision ||
                                                    t.reference_epoch < cached->message.timing.reference_epoch)) ||
                         (state.has_pose && t.instance_id == state.timing.instance_id &&
                          (t.metadata_revision < state.timing.metadata_revision ||
                           t.reference_epoch < state.timing.reference_epoch ||
                           (t.source_session_id != state.timing.source_session_id &&
                            t.metadata_revision <= state.timing.metadata_revision)));
        if (old) {
            ++state.rejected_packets;
            return;
        }
        if (cached != cache.end()) {
            cache.erase(cached);
        } else if (cache.size() >= 17U) {
            const auto oldest_candidate =
                std::ranges::find_if(cache, [this](const auto& value) { return !matches_current_instance(value); });
            cache.erase(oldest_candidate);
        }
        cache.push_back(CachedTelemetry{message, now});
        ++state.telemetry_packets;
        update_metadata();
    }

    void accept_message(const realtime::HeadTrackingMessage& message, Clock::time_point now) {
        using Kind = realtime::HeadTrackingMessageKind;
        if (message.kind == Kind::incompatible) {
            ++state.rejected_packets;
            ++state.protocol_mismatches;
            error_message = "PoseBridge protocol mismatch: receiver requires protocol 3";
            return;
        }
        if ((!requested_source.empty() && message.source_id != requested_source) ||
            (state.has_pose && message.source_id != state.source_id)) {
            ++state.ignored_sources;
            return;
        }
        if (message.kind != Kind::pose) {
            accept_telemetry(message, now);
            return;
        }
        if (!source_order.accept(message.timing)) {
            ++state.rejected_packets;
            return;
        }
        if (state.has_pose && now - received_at >= k_stale_after) {
            ++state.recovery_count;
        }
        state.source_id = message.source_id;
        state.has_pose = true;
        state.orientation = message.orientation;
        state.timing = message.timing;
        source_allows_pose = true;
        received_at = now;
        state.received_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_at).count());
        state.missing_tx_packets +=
            std::min(source_order.last_gap(), std::numeric_limits<std::uint64_t>::max() - state.missing_tx_packets);
        ++state.sequence;
        ++state.pose_packets;
        state.state = OscHeadTrackingState::active;
        update_metadata();
    }

    void receive(const std::stop_token& token) noexcept {
        try {
            std::array<std::byte, 65536> buffer{}; // full UDP datagram: an oversized valid prefix must not be accepted
            while (!token.stop_requested()) {
#ifdef _WIN32
                WSAPOLLFD descriptor{socket, POLLIN, 0};
                const int ready = WSAPoll(&descriptor, 1U, 20);
#else
                pollfd descriptor{socket, POLLIN, 0};
                const int ready = ::poll(&descriptor, 1U, 20);
#endif
                if (ready < 0) {
                    const int error = socket_error();
                    if (retryable(error)) {
                        continue;
                    }
                    (void) fail("OSC UDP 等待失败", error);
                    return;
                }
                if (ready == 0) {
                    continue;
                }
                if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    (void) fail("OSC UDP socket 不可用", descriptor.revents);
                    return;
                }
                sockaddr_in peer{};
                AddressSize peer_size = sizeof(peer);
                const auto size = ::recvfrom(socket,
                                             reinterpret_cast<char*>(buffer.data()),
                                             static_cast<int>(buffer.size()),
                                             0,
                                             reinterpret_cast<sockaddr*>(&peer),
                                             &peer_size);
                if (size < 0) {
                    const int error = socket_error();
                    if (retryable(error)) {
                        continue;
                    }
                    (void) fail("OSC UDP 接收失败", error);
                    return;
                }
                const auto now = Clock::now();
                const auto pose = realtime::decode_head_tracking_osc(
                    std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(size)});
                const bool loopback = (ntohl(peer.sin_addr.s_addr) & 0xff000000U) == 0x7f000000U;
                const std::lock_guard lock(mutex);
                ++state.packets_received;
                if (!loopback || !pose) {
                    ++state.rejected_packets;
                    continue;
                }
                accept_message(*pose, now);
            }
        } catch (...) {
            // No exceptions escape the worker, including allocation failures while
            // formatting a socket error. The fixed-size failure state remains queryable.
            const std::lock_guard lock(mutex);
            state.state = OscHeadTrackingState::failed;
        }
    }
};

OscHeadTrackingReceiver::OscHeadTrackingReceiver(std::uint16_t port, std::string source_id)
    : impl_(std::make_unique<Impl>(port, std::move(source_id))) {}

OscHeadTrackingReceiver::~OscHeadTrackingReceiver() {
    stop();
}

Result<void> OscHeadTrackingReceiver::start() {
    if (!impl_->requested_source.empty() && !realtime::valid_source_id(impl_->requested_source)) {
        return make_error(ErrorCode::invalid_argument, "source_id must be 1..256 valid UTF-8 bytes without controls");
    }
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->state.state == OscHeadTrackingState::waiting || impl_->state.state == OscHeadTrackingState::active) {
            return {};
        }
    }
    stop();
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->state = {};
        impl_->source_order = {};
        impl_->state.source_id = impl_->requested_source;
        impl_->info_cache.clear();
        impl_->status_cache.clear();
        impl_->source_allows_pose = true;
        impl_->error_message.clear();
    }
#ifdef _WIN32
    WSADATA data{};
    const int startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup != 0) {
        return impl_->fail("OSC WinSock 初始化失败", startup);
    }
    impl_->winsock_started = true;
    impl_->socket = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#else
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
    // Bind failures leave the handle reusable and never silently choose another port.
    const auto failed = [this](const char* message) -> Result<void> {
        const int error = socket_error();
        impl_->close();
        return impl_->fail(message, error);
    };
    if (impl_->socket == k_invalid_socket) {
        return failed("OSC UDP socket 创建失败");
    }
#ifdef _WIN32
    const BOOL exclusive = TRUE;
    u_long nonblocking = 1;
    if (setsockopt(impl_->socket,
                   SOL_SOCKET,
                   SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof(exclusive)) != 0 ||
        ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0) {
        return failed("OSC UDP socket 配置失败");
    }
#else
    // fcntl is the POSIX API for these per-descriptor flags.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int nonblocking_result = ::fcntl(impl_->socket, F_SETFL, O_NONBLOCK);
    if (nonblocking_result < 0) {
        return failed("OSC UDP socket 配置失败");
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int inheritance_result = ::fcntl(impl_->socket, F_SETFD, FD_CLOEXEC);
    if (inheritance_result < 0) {
        return failed("OSC UDP socket 配置失败");
    }
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(impl_->requested_port);
    if (::bind(impl_->socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return failed("OSC 回环端口绑定失败，端口可能已被占用");
    }
    AddressSize address_size = sizeof(address);
    if (::getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        return failed("OSC 监听端口查询失败");
    }
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->started_at = Clock::now();
        impl_->state.session_id = allocate_session_id();
        impl_->state.bound_port = ntohs(address.sin_port);
        impl_->state.state = OscHeadTrackingState::waiting;
    }
    try {
        impl_->worker = std::jthread([state = impl_.get()](const std::stop_token& token) { state->receive(token); });
    } catch (...) {
        impl_->close();
        return impl_->fail("OSC 接收线程创建失败", 0, ErrorCode::internal_error);
    }
    return {};
}

void OscHeadTrackingReceiver::stop() noexcept {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->close();
    const std::lock_guard lock(impl_->mutex);
    impl_->state.state = OscHeadTrackingState::stopped;
    impl_->state.bound_port = 0;
}

OscHeadTrackingSnapshot OscHeadTrackingReceiver::snapshot() const {
    const std::lock_guard lock(impl_->mutex);
    auto result = impl_->state;
    if (result.has_pose) {
        const auto age = Clock::now() - impl_->received_at;
        result.age_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        result.fresh = result.state == OscHeadTrackingState::active && impl_->source_allows_pose &&
                       age + std::chrono::nanoseconds{result.timing.source_age_at_send_ns} < k_stale_after;
        if (result.state == OscHeadTrackingState::active && !result.fresh) {
            result.state = OscHeadTrackingState::stale;
        }
    }
    if (result.has_heartbeat) {
        const auto age = Clock::now() - impl_->heartbeat_at;
        result.heartbeat_age_ms =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        result.heartbeat_alive =
            (result.state == OscHeadTrackingState::waiting || result.state == OscHeadTrackingState::active ||
             result.state == OscHeadTrackingState::stale) &&
            age < std::chrono::seconds{3};
    }
    return result;
}

std::string OscHeadTrackingReceiver::snapshot_json() const {
    const auto s = snapshot();
    const auto decimal = [](auto value) { return std::to_string(value); };
    const auto& t = s.timing;
    nlohmann::json result{
        {"schema", realtime::k_posebridge_protocol},
        {"state", static_cast<std::int32_t>(s.state)},
        {"bound_port", s.bound_port},
        {"source_id", s.source_id},
        {"has_pose", s.has_pose},
        {"fresh", s.fresh},
        {"receiver_session_id", decimal(s.session_id)},
        {"receiver_sequence", decimal(s.sequence)},
        {"received_ns", decimal(s.received_ns)},
        {"age_ms", decimal(s.age_ms)},
        {"packets_received", decimal(s.packets_received)},
        {"rejected_packets", decimal(s.rejected_packets)},
        {"pose_packets", decimal(s.pose_packets)},
        {"telemetry_packets", decimal(s.telemetry_packets)},
        {"ignored_sources", decimal(s.ignored_sources)},
        {"protocol_mismatches", decimal(s.protocol_mismatches)},
        {"missing_tx_packets", decimal(s.missing_tx_packets)},
        {"recovery_count", decimal(s.recovery_count)},
        {"has_heartbeat", s.has_heartbeat},
        {"heartbeat_alive", s.heartbeat_alive},
        {"heartbeat_age_ms", decimal(s.heartbeat_age_ms)},
        {"info_matches_pose", s.info_matches_pose},
        {"status_matches_pose", s.status_matches_pose},
        {"info", s.info_json.empty() ? nlohmann::json{} : nlohmann::json::parse(s.info_json)},
        {"source_status", s.status_json.empty() ? nlohmann::json{} : nlohmann::json::parse(s.status_json)}};
    if (s.has_pose) {
        result["pose"] = {{"quaternion_xyzw", s.orientation.quaternion_xyzw},
                          {"euler_deg", s.orientation.euler_deg},
                          {"instance_id", decimal(t.instance_id)},
                          {"session_id", decimal(t.source_session_id)},
                          {"sequence", decimal(t.source_sequence)},
                          {"tx_sequence", decimal(t.tx_sequence)},
                          {"reference_epoch", decimal(t.reference_epoch)},
                          {"metadata_revision", decimal(t.metadata_revision)},
                          {"received_ns", decimal(t.source_received_ns)},
                          {"age_at_send_ns", decimal(t.source_age_at_send_ns)},
                          {"sample_time_kind", t.sample_time_kind},
                          {"sample_time_ms", decimal(t.sample_time_ms)},
                          {"sample_clock_epoch", decimal(t.sample_clock_epoch)}};
    } else {
        result["pose"] = nullptr;
    }
    return result.dump();
}

std::string OscHeadTrackingReceiver::last_error() const {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->state.state == OscHeadTrackingState::failed && impl_->error_message.empty()) {
        return "OSC 接收线程异常终止";
    }
    return impl_->error_message;
}

} // namespace mradm
