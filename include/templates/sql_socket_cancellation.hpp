#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ioapiset.h>
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <optional>
#include <stop_token>

namespace caf_plugin_system::sql_backend {

// Interrupt blocking I/O without touching the driver's native connection from
// another thread. The callback owns a DUPLICATE descriptor: a driver may close
// its original descriptor internally, so retaining that numeric value alone
// would risk interrupting an unrelated socket after descriptor reuse.
class SocketCancellation {
public:
#ifdef _WIN32
    using socket_type = SOCKET;
    static constexpr socket_type invalid_socket = INVALID_SOCKET;
#else
    using socket_type = int;
    static constexpr socket_type invalid_socket = -1;
#endif

    SocketCancellation(socket_type socket, std::stop_token stop)
        : socket_(duplicate(socket)) {
        if (valid())
            callback_.emplace(stop, ShutdownSocket{socket_.value});
    }

    SocketCancellation(const SocketCancellation&) = delete;
    SocketCancellation& operator=(const SocketCancellation&) = delete;

    bool valid() const noexcept { return socket_.value != invalid_socket; }

    // Member destruction order matters: stop_callback unregisters and waits
    // for an in-flight callback BEFORE OwnedSocket closes its descriptor.
    // The driver connection itself remains owned by its worker thread.
    ~SocketCancellation() = default;

private:
    struct OwnedSocket {
        socket_type value;
        ~OwnedSocket() {
            if (value == invalid_socket)
                return;
#ifdef _WIN32
            closesocket(value);
#else
            ::close(value);
#endif
        }
    };

    struct ShutdownSocket {
        socket_type socket;
        void operator()() const noexcept {
#ifdef _WIN32
            ::shutdown(socket, SD_BOTH);
            // shutdown prevents NEW I/O, but does not wake an already pending
            // Winsock receive on Windows. Cancel the old endpoint's pending I/O
            // through our duplicate, never through a possibly reused driver fd.
            ::CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
#else
            ::shutdown(socket, SHUT_RDWR);
#endif
        }
    };

    static socket_type duplicate(socket_type socket) noexcept {
        if (socket == invalid_socket)
            return invalid_socket;
#ifdef _WIN32
        WSAPROTOCOL_INFOW protocol{};
        if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &protocol) != 0)
            return invalid_socket;
        // The driver's TCP sockets are created with socket(), whose duplicate
        // must retain WSA_FLAG_OVERLAPPED. Do not inherit the extra descriptor.
        return WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                          FROM_PROTOCOL_INFO, &protocol, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
        return ::fcntl(socket, F_DUPFD_CLOEXEC, 0);
#endif
    }

    OwnedSocket socket_;
    std::optional<std::stop_callback<ShutdownSocket>> callback_;
};

} // namespace caf_plugin_system::sql_backend
