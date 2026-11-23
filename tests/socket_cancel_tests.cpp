#include "net/tcp_socket.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct SocketHandle {
    SOCKET value = INVALID_SOCKET;

    ~SocketHandle() {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
    }
};

}  // namespace

int main() {
    try {
        // TcpSocket 先初始化 Winsock，并最后析构。
        rtsp::TcpSocket client;
        SocketHandle listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        check(listener.value != INVALID_SOCKET, "cannot create listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(
            bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "cannot bind listener"
        );
        check(listen(listener.value, 2) == 0, "cannot listen");
        int size = sizeof(address);
        check(
            getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0,
            "cannot get listener port"
        );
        const auto port = ntohs(address.sin_port);
        client.connect("127.0.0.1", port, 1000);
        SocketHandle peer{accept(listener.value, nullptr, nullptr)};
        check(peer.value != INVALID_SOCKET, "cannot accept");
        const int receive_buffer = 1024;
        check(
            setsockopt(
                peer.value, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer),
                sizeof(receive_buffer)
            ) == 0,
            "cannot limit receive buffer"
        );
        const std::string payload(16 * 1024 * 1024, 'x');
        const auto started = std::chrono::steady_clock::now();
        bool cancelled = false;
        try {
            // 对端不读，发送最终进入不可写等待；十分钟期限不能拖住取消。
            while (!cancelled) {
                client.send_all(payload, 600000, [&] {
                    return std::chrono::steady_clock::now() - started
                           >= std::chrono::milliseconds(100);
                });
            }
        } catch (const rtsp::StopRequested&) {
            cancelled = true;
        }
        check(cancelled, "blocked send ignored cancellation");
        check(
            std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
            "send cancellation too slow"
        );
        client.close();
        std::cout << "PASS: blocked TCP send cancels with large timeout\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
