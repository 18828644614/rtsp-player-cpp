#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rtsp {

enum class ReadStatus {
    Data,
    Timeout,
    Eof
};

struct ReadResult {
    ReadStatus status;
    std::size_t size = 0;
};

// 一个对象拥有一个连接；所有等待都有期限，析构时释放 Winsock。
class TcpSocket {
public:
    TcpSocket();
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    void connect(const std::string& host, std::uint16_t port, int timeout_ms);
    void send_all(const std::string& data, int timeout_ms);
    ReadResult receive(char* data, std::size_t capacity, int timeout_ms);

    bool is_open() const {
        return handle_ != INVALID_SOCKET;
    }

    void close();

private:
    bool wait(bool writing, std::chrono::steady_clock::time_point deadline);
    SOCKET handle_ = INVALID_SOCKET;
};

}  // namespace rtsp
