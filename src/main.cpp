#include "rtsp/rtsp_client.h"
#include "media/video_decoder.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}
#include <windows.h>

#include <atomic>
#include <iostream>
#include <exception>
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
    std::cout << "rtsp_player: H264 decoding with dual-track RTP/RTCP statistics (P2)\n"
                 "Usage: rtsp_player <rtsp://IPv4[:port]/live> [--seconds N] [--timeout-ms N]\n"
                 "  --seconds N     Receive for N seconds after PLAY (0 = until Ctrl+C).\n"
                 "  --timeout-ms N  Connect/request timeout, 1..600000 (default 3000).\n"
                 "Numeric IPv4 and localhost are supported. FFmpeg avcodec/avutil required.\n";
}

void print_video_frame(const AVFrame& frame, std::uint64_t number) {
    const auto* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame.format));
    std::cout << "[VIDEO] frame=" << number << " pts=" << frame.pts << " width=" << frame.width
              << " height=" << frame.height << " format=" << (format ? format : "unknown") << '\n';
}

void run_player(const std::string& url, const rtsp::ClientOptions& options) {
    std::uint64_t frames = 0;
    rtsp::VideoDecoder decoder([&frames](const AVFrame& frame) {
        print_video_frame(frame, ++frames);
    });
    rtsp::RtspClient client(options, std::cout, [] {
        return stopped.load(std::memory_order_relaxed);
    });
    client.set_video_handler([&decoder](const rtsp::H264AccessUnit& au) {
        const bool decoded = decoder.decode(au);
        if (!decoded) {
            std::cerr << "[WARN] H264 decoder rejected AU; waiting for IDR\n";
        }
        return decoded;
    });

    // 断连或控制请求失败时，也排空已经提交的完整 AU，并保留原始错误。
    std::exception_ptr failure;
    try {
        client.run(url);
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        if (!decoder.drain()) {
            std::cerr << "[WARN] corrupt H264 data while draining decoder\n";
        }
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    const auto& stats = decoder.stats();
    std::cout << "[FINAL] decode frames=" << stats.frames << " errors=" << stats.errors
              << " resets=" << stats.resets << std::endl;
    if (failure) {
        std::rethrow_exception(failure);
    }
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

        run_player(url, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
