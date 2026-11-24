#include "player/player.h"
#include <windows.h>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free, "console stop flag must be lock-free");

BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    stopped.store(true, std::memory_order_relaxed);
    return TRUE;
}

int number(const std::string& value, int maximum) {
    int result = 0;
    if (value.empty()) {
        throw std::runtime_error("empty numeric option");
    }
    for (char c : value) {
        const int digit = c - '0';
        if (digit < 0 || digit > 9 || result > (maximum - digit) / 10) {
            throw std::runtime_error("invalid numeric option: " + value);
        }
        result = result * 10 + digit;
    }
    return result;
}

void help() {
    std::cout
        << "rtsp_player: SDL3 video/audio with dual-track RTP/RTCP statistics (P5)\n"
           "Usage: rtsp_player <rtsp://IPv4[:port]/live> [--seconds N] [--timeout-ms N]\n"
           "  --seconds N     Receive for N seconds after PLAY (0 = until window close or Ctrl+C).\n"
           "  --timeout-ms N  Connect/request timeout, 1..600000 (default 3000).\n"
           "Numeric IPv4 and localhost are supported. FFmpeg avcodec/avutil and SDL3 required.\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 1 || (argc == 2 && std::string(argv[1]) == "--help")) {
        help();
        return 0;
    }

    try {
        rtsp::ClientOptions options;
        const std::string url = argv[1];

        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc) {
                throw std::runtime_error("option requires a value");
            }

            const std::string option = argv[i];
            if (option == "--seconds") {
                options.seconds = number(argv[i + 1], 86400);
            } else if (option == "--timeout-ms") {
                options.timeout_ms = number(argv[i + 1], 600000);
            } else {
                throw std::runtime_error("unknown option: " + option);
            }
        }

        if (options.timeout_ms == 0) {
            throw std::runtime_error("timeout must be positive");
        }
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            throw std::runtime_error("cannot install console handler");
        }

        rtsp::Player player(options, [] { return stopped.load(std::memory_order_relaxed); });
        player.run(url);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
