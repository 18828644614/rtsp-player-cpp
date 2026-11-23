#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace rtsp {

// 只做视频 PTS 到单调时钟的换算；不读取系统时间，便于确定性测试。
struct VideoClock {
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;

    Time deadline(std::int64_t pts, Time now) {
        // 解码输出已按显示顺序排列。大幅跳变重新锚定，避免长时间停画。
        if (!origin || pts < last_pts || pts - last_pts > 180000) {
            if (origin) {
                ++resets;
            }
            origin = now;
            origin_pts = pts;
        }
        last_pts = pts;
        return *origin + std::chrono::microseconds((pts - origin_pts) * 100 / 9);
    }

    std::optional<Time> origin;
    std::int64_t origin_pts = 0;
    std::int64_t last_pts = 0;
    std::uint64_t resets = 0;
};

}  // namespace rtsp
