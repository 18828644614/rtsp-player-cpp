#include "net/tcp_socket.h"
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <stdexcept>

namespace rtsp {
namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void socket_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed, Winsock=" + std::to_string(WSAGetLastError())
    );
}

}  // namespace

TcpSocket::TcpSocket() {
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
    }
}

TcpSocket::~TcpSocket() {
    close();
    WSACleanup();
}

bool TcpSocket::wait(bool writing, Clock::time_point deadline) {
    while (true) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        fd_set ready, errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(handle_, &ready);
        FD_SET(handle_, &errors);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
        const int result =
            select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout);
        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            socket_error("select");
        }
        return result > 0;
    }
}

void TcpSocket::connect(const std::string& host, std::uint16_t port, int timeout_ms) {
    if (is_open()) {
        throw std::runtime_error("socket is already connected");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    const auto ip = host == "localhost" ? std::string("127.0.0.1") : host;
    if (InetPtonA(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("P1 supports numeric IPv4 and localhost only");
    }
    handle_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!is_open()) {
        socket_error("socket");
    }
    u_long nonblocking = 1;
    if (ioctlsocket(handle_, FIONBIO, &nonblocking) != 0) {
        socket_error("ioctlsocket");
    }
    if (::connect(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address))
        == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            socket_error("connect");
        }
        if (!wait(true, Clock::now() + std::chrono::milliseconds(timeout_ms))) {
            throw std::runtime_error("TCP connect timeout");
        }
        int error = 0;
        int size = sizeof(error);
        if (getsockopt(handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size)
            != 0) {
            socket_error("getsockopt");
        }
        if (error != 0) {
            throw std::runtime_error("TCP connect failed, Winsock=" + std::to_string(error));
        }
    }
}

void TcpSocket::send_all(const std::string& data, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (!wait(true, deadline)) {
            throw std::runtime_error("TCP send timeout");
        }
        const int count = send(
            handle_, data.data() + sent,
            static_cast<int>(std::min(data.size() - sent, static_cast<std::size_t>(INT_MAX))), 0
        );
        if (count == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            socket_error("send");
        }
        if (count == 0) {
            throw std::runtime_error("TCP send returned zero");
        }
        sent += static_cast<std::size_t>(count);
    }
}

ReadResult TcpSocket::receive(char* data, std::size_t capacity, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (wait(false, deadline)) {
        const int count = recv(
            handle_, data, static_cast<int>(std::min(capacity, static_cast<std::size_t>(INT_MAX))),
            0
        );
        if (count == 0) {
            return {ReadStatus::Eof, 0};
        }
        if (count == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            socket_error("recv");
        }
        return {ReadStatus::Data, static_cast<std::size_t>(count)};
    }
    return {ReadStatus::Timeout, 0};
}

void TcpSocket::close() {
    if (is_open()) {
        closesocket(handle_);
        handle_ = INVALID_SOCKET;
    }
}

}  // namespace rtsp
