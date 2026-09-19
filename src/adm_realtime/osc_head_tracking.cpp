#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

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
    explicit Impl(std::uint16_t port) : requested_port(port) {}

  private:
    friend class OscHeadTrackingReceiver;
    std::uint16_t requested_port;
    Socket socket{k_invalid_socket};
#ifdef _WIN32
    bool winsock_started{false};
#endif
    std::jthread worker;
    mutable std::mutex mutex;
    OscHeadTrackingSnapshot state;
    Clock::time_point started_at;
    Clock::time_point received_at;
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
                if (state.has_pose && now - received_at >= k_stale_after) {
                    ++state.recovery_count;
                }
                received_at = now;
                state.received_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_at).count());
                state.has_pose = true;
                ++state.sequence;
                state.orientation = *pose;
                state.state = OscHeadTrackingState::active;
            }
        } catch (...) {
            // No exceptions escape the worker, including allocation failures while
            // formatting a socket error. The fixed-size failure state remains queryable.
            const std::lock_guard lock(mutex);
            state.state = OscHeadTrackingState::failed;
        }
    }
};

OscHeadTrackingReceiver::OscHeadTrackingReceiver(std::uint16_t port) : impl_(std::make_unique<Impl>(port)) {}

OscHeadTrackingReceiver::~OscHeadTrackingReceiver() {
    stop();
}

Result<void> OscHeadTrackingReceiver::start() {
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
        result.fresh = result.state == OscHeadTrackingState::active && age < k_stale_after;
        if (result.state == OscHeadTrackingState::active && !result.fresh) {
            result.state = OscHeadTrackingState::stale;
        }
    }
    return result;
}

std::string OscHeadTrackingReceiver::last_error() const {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->state.state == OscHeadTrackingState::failed && impl_->error_message.empty()) {
        return "OSC 接收线程异常终止";
    }
    return impl_->error_message;
}

} // namespace mradm
