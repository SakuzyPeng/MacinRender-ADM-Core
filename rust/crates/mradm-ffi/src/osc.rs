use mradm_osc::receiver;

// ── Loopback receiver (formerly C++ `OscHeadTrackingReceiver`) ──────────────────────────────

#[cfg(not(windows))]
const BIND: receiver::Bind = receiver::bind_loopback;
#[cfg(windows)]
const BIND: receiver::Bind = winsock::bind;

/// A loopback receiver on `port` for `source` (may be empty; `start` checks it), bound as the
/// platform requires (Windows: `SO_EXCLUSIVEADDRUSE`).
pub fn receiver(port: u16, source: &[u8]) -> receiver::Receiver {
    receiver::Receiver::new(port, source, BIND)
}

/// The Windows bind keeps the former receiver's `SO_EXCLUSIVEADDRUSE`, which `std::net` cannot
/// set before binding: no other socket can share the port, even one asking for address reuse.
#[cfg(windows)]
mod winsock {
    use super::receiver::{BindError, Bound};
    use std::net::UdpSocket;
    use std::os::windows::io::{FromRawSocket, RawSocket};

    type Socket = usize;
    const INVALID_SOCKET: Socket = !0;
    const AF_INET: i32 = 2;
    const SOCK_DGRAM: i32 = 2;
    const IPPROTO_UDP: i32 = 17;
    const SOL_SOCKET: i32 = 0xffff;
    const SO_EXCLUSIVEADDRUSE: i32 = !4;
    // Receive timeouts need an overlapped socket (as `std::net` creates them).
    const WSA_FLAG_OVERLAPPED: u32 = 0x01;
    const WSA_FLAG_NO_HANDLE_INHERIT: u32 = 0x80;

    #[repr(C)]
    struct SockaddrIn {
        family: u16,
        port: u16,
        address: u32,
        zero: [u8; 8],
    }

    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn WSAStartup(version: u16, data: *mut u64) -> i32;
        fn WSACleanup() -> i32;
        fn WSAGetLastError() -> i32;
        fn WSASocketW(
            family: i32,
            kind: i32,
            protocol: i32,
            info: *const u8,
            group: u32,
            flags: u32,
        ) -> Socket;
        fn setsockopt(socket: Socket, level: i32, name: i32, value: *const u8, len: i32) -> i32;
        #[link_name = "bind"]
        fn bind_socket(socket: Socket, address: *const SockaddrIn, len: i32) -> i32;
        fn closesocket(socket: Socket) -> i32;
    }

    /// Balances this receiver's `WSAStartup`; dropped after its socket.
    struct WinSock;

    impl Drop for WinSock {
        fn drop(&mut self) {
            unsafe { WSACleanup() };
        }
    }

    pub(super) fn bind(port: u16) -> Result<Bound, BindError> {
        // WSADATA is 408 bytes on 64-bit Windows.
        let mut data = [0u64; 64];
        let startup = unsafe { WSAStartup(0x0202, data.as_mut_ptr()) };
        if startup != 0 {
            return Err(BindError {
                message: "OSC WinSock 初始化失败",
                code: startup,
            });
        }
        let winsock = WinSock;
        let fail = |message: &'static str| BindError {
            message,
            code: unsafe { WSAGetLastError() },
        };
        let socket = unsafe {
            WSASocketW(
                AF_INET,
                SOCK_DGRAM,
                IPPROTO_UDP,
                std::ptr::null(),
                0,
                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT,
            )
        };
        if socket == INVALID_SOCKET {
            return Err(fail("OSC UDP socket 创建失败"));
        }
        let close = |error: BindError| {
            unsafe { closesocket(socket) };
            Err(error)
        };
        let exclusive: i32 = 1;
        let configured = unsafe {
            setsockopt(
                socket,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                (&raw const exclusive).cast(),
                4,
            )
        };
        if configured != 0 {
            return close(fail("OSC UDP socket 配置失败"));
        }
        let address = SockaddrIn {
            family: AF_INET as u16,
            port: port.to_be(),
            address: u32::from(std::net::Ipv4Addr::LOCALHOST).to_be(),
            zero: [0; 8],
        };
        let size = std::mem::size_of::<SockaddrIn>() as i32;
        if unsafe { bind_socket(socket, &address, size) } != 0 {
            return close(fail("OSC 回环端口绑定失败，端口可能已被占用"));
        }
        // SAFETY: a valid, bound socket that the returned `UdpSocket` now owns and closes.
        let socket = unsafe { UdpSocket::from_raw_socket(socket as RawSocket) };
        Ok(Bound {
            socket,
            guard: Some(Box::new(winsock)),
        })
    }
}
