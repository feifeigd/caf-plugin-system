#include "templates/sql_socket_cancellation.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <barrier>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace sql = caf_plugin_system::sql_backend;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class NetworkRuntime {
public:
    NetworkRuntime() {
#ifdef _WIN32
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
#endif
    }
    ~NetworkRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class Socket {
public:
    using native_type = sql::SocketCancellation::socket_type;
    static constexpr native_type invalid = sql::SocketCancellation::invalid_socket;

    explicit Socket(native_type value = invalid) : value_(value) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value_(std::exchange(other.value_, invalid)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, invalid);
        }
        return *this;
    }
    native_type get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != invalid; }
    void reset(native_type value = invalid) noexcept {
        if (valid()) {
#ifdef _WIN32
            closesocket(value_);
#else
            ::close(value_);
#endif
        }
        value_ = value;
    }
    void bound_waits() const {
#ifdef _WIN32
        const DWORD timeout = 2000;
#else
        const timeval timeout{2, 0};
#endif
        require(setsockopt(value_, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "cannot bound receive timeout");
        require(setsockopt(value_, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "cannot bound send timeout");
    }

private:
    native_type value_;
};

class Listener {
public:
    Listener() : socket_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        require(socket_.valid(), "cannot create listener");
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_.sin_port = 0;
        require(::bind(socket_.get(), reinterpret_cast<const sockaddr*>(&address_),
                       sizeof(address_)) == 0, "cannot bind loopback listener");
        require(::listen(socket_.get(), 1) == 0, "cannot listen on loopback");
#ifdef _WIN32
        int length = sizeof(address_);
#else
        socklen_t length = sizeof(address_);
#endif
        require(::getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&address_),
                              &length) == 0, "cannot read ephemeral loopback port");
    }
    Socket::native_type get() const noexcept { return socket_.get(); }
    const sockaddr_in& address() const noexcept { return address_; }

private:
    Socket socket_;
    sockaddr_in address_{};
};

class TcpPair {
public:
    TcpPair() {
        Listener listener;
        connect(listener);
    }
    explicit TcpPair(const Listener& listener) { connect(listener); }

    Socket client;
    Socket peer;

private:
    void connect(const Listener& listener) {
        client.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        require(client.valid(), "cannot create client socket");
        client.bound_waits();
        require(::connect(client.get(), reinterpret_cast<const sockaddr*>(&listener.address()),
                          sizeof(sockaddr_in)) == 0, "cannot connect loopback client");
        peer.reset(::accept(listener.get(), nullptr, nullptr));
        require(peer.valid(), "cannot accept loopback client");
        peer.bound_waits();
    }
};

class PendingRead {
public:
    explicit PendingRead(Socket::native_type socket) {
        auto entered = std::make_shared<std::promise<void>>();
        auto starting = entered->get_future();
        result_ = std::async(std::launch::async, [entered, socket] {
            char value = 0;
            entered->set_value();
            return static_cast<int>(::recv(socket, &value, 1, 0));
        });
        require(starting.wait_for(1s) == std::future_status::ready,
                "read thread did not start");
    }
    void require_blocked() {
        require(result_.wait_for(75ms) == std::future_status::timeout,
                "receive did not block before cancellation");
    }
    void require_interrupted() {
        // SO_RCVTIMEO is a two-second fallback if this assertion fails, so a
        // broken cancellation cannot strand the test in future destruction.
        require(result_.wait_for(1s) == std::future_status::ready,
                "cancellation did not promptly interrupt receive");
        require(result_.get() <= 0, "cancelled receive unexpectedly read data");
    }

private:
    std::future<int> result_;
};

void exchange_byte(const Socket& sender, const Socket& receiver, char expected) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    require(::send(sender.get(), &expected, 1, flags) == 1,
            "unrelated/live socket send was damaged");
    char actual = 0;
    require(::recv(receiver.get(), &actual, 1, 0) == 1 && actual == expected,
            "unrelated/live socket receive was damaged");
}

void verify_blocking_read_cancellation() {
    TcpPair sockets;
    std::stop_source stop;
    sql::SocketCancellation cancellation{sockets.client.get(), stop.get_token()};
    require(cancellation.valid(), "cannot duplicate connected socket");
    PendingRead read{sockets.client.get()};
    read.require_blocked();
    require(stop.request_stop(), "first stop request was not accepted");
    read.require_interrupted();
    require(!stop.request_stop(), "stop request was not idempotent");
}

void verify_stop_before_registration() {
    TcpPair sockets;
    std::stop_source stop;
    stop.request_stop();
    sql::SocketCancellation cancellation{sockets.client.get(), stop.get_token()};
    require(cancellation.valid(), "cannot register an already-stopped token");
    PendingRead read{sockets.client.get()};
    read.require_interrupted();
}

void verify_unregister_preserves_original() {
    TcpPair sockets;
    std::stop_source stop;
    {
        sql::SocketCancellation cancellation{sockets.client.get(), stop.get_token()};
        require(cancellation.valid(), "cannot register cancellation");
    }
    stop.request_stop();
    exchange_byte(sockets.peer, sockets.client, 'a');
    exchange_byte(sockets.client, sockets.peer, 'b');
}

void verify_closed_original_preserves_new_connection() {
    TcpPair original;
    Listener replacement_listener;
    std::stop_source stop;
    sql::SocketCancellation cancellation{original.client.get(), stop.get_token()};
    require(cancellation.valid(), "cannot duplicate old connection");
    const auto old_number = original.client.get();
    original.client.reset();
    TcpPair replacement{replacement_listener};
#ifndef _WIN32
    // POSIX permits forcing the exact numeric descriptor reuse. Windows
    // chooses SOCKET values itself; both platforms still verify that the
    // duplicate targets the old endpoint and leaves the new endpoint intact.
    if (replacement.client.get() != old_number) {
        const auto reused = ::dup2(replacement.client.get(), old_number);
        require(reused == old_number, "cannot force descriptor reuse");
        replacement.client.reset(reused);
    }
#endif
    PendingRead old_peer{original.peer.get()};
    old_peer.require_blocked();
    stop.request_stop();
    old_peer.require_interrupted();
    exchange_byte(replacement.peer, replacement.client, 'c');
    exchange_byte(replacement.client, replacement.peer, 'd');
}

void verify_concurrent_unregister_and_stop() {
    TcpPair unrelated;
    for (int attempt = 0; attempt < 32; ++attempt) {
        TcpPair sockets;
        std::stop_source stop;
        auto cancellation = std::make_unique<sql::SocketCancellation>(
            sockets.client.get(), stop.get_token());
        require(cancellation->valid(), "cannot register concurrent cancellation");
        std::barrier gate{2};
        auto stopping = std::async(std::launch::async, [&] {
            gate.arrive_and_wait();
            stop.request_stop();
        });
        gate.arrive_and_wait();
        cancellation.reset();
        stopping.get();
        // Either shutdown or unregistration may win. No live, unrelated
        // descriptor may be interrupted and no callback may outlive its fd.
        exchange_byte(unrelated.peer, unrelated.client, 'e');
        exchange_byte(unrelated.client, unrelated.peer, 'f');
    }
}

} // namespace

int main() {
    try {
        NetworkRuntime network;
        verify_blocking_read_cancellation();
        verify_stop_before_registration();
        verify_unregister_preserves_original();
        verify_closed_original_preserves_new_connection();
        verify_concurrent_unregister_and_stop();
        std::stop_source stop;
        sql::SocketCancellation invalid{Socket::invalid, stop.get_token()};
        require(!invalid.valid(), "invalid socket was accepted");
        stop.request_stop();
        std::cout << "sql socket cancellation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
