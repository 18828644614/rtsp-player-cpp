#pragma once

#include "rtp/packet_stats.h"

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace rtsp {

// 纯时间换算；调用方负责加锁。不使用本机墙上时钟，也不按收包时间归零。
struct SyncClock {
    enum class Track {
        Video,
        Audio
    };
    enum class Action {
        Wait,
        Present,
        Drop
    };

    struct Mapping {
        SenderReport report;
        int rate = 0;
    };

    static std::int64_t rtp_delta(std::uint32_t value, std::uint32_t origin) {
        const auto delta = value - origin;
        return delta <= 0x7fffffffU ? static_cast<std::int64_t>(delta)
                                    : static_cast<std::int64_t>(delta) - 0x100000000LL;
    }

    static std::int64_t ntp_delta_us(std::uint64_t value, std::uint64_t origin) {
        // 先作相对差，再拆分秒和小数，避免绝对 NTP 转换的溢出与精度损失。
        const bool negative = value < origin;
        const auto delta = negative ? origin - value : value - origin;
        const auto us = static_cast<std::int64_t>(delta >> 32) * 1000000
                        + static_cast<std::int64_t>((delta & 0xffffffffULL) * 1000000 >> 32);
        return negative ? -us : us;
    }

    bool ready() const {
        return video.has_value() && audio.has_value();
    }

    std::optional<std::int64_t> time(Track track, std::int64_t pts) const {
        const auto& mapping = track == Track::Video ? video : audio;
        if (!mapping) {
            return std::nullopt;
        }
        return ntp_delta_us(mapping->report.ntp, *origin_ntp)
               + rtp_delta(static_cast<std::uint32_t>(pts), mapping->report.rtp_timestamp) * 1000000
                     / mapping->rate;
    }

    // 返回 true 表示时间轴跳变：调用方清空旧缓冲，等待另一轨的新 SR。
    bool update(Track track, int rate, const SenderReport& report) {
        if (rate != (track == Track::Video ? 90000 : 48000)) {
            throw std::runtime_error("unsupported SR clock rate");
        }
        auto& mapping = track == Track::Video ? video : audio;
        if (mapping && report.ntp <= mapping->report.ntp) {
            return false;  // 重复或过期的 SR 不移动当前时间轴。
        }
        bool reset = false;
        if (mapping) {
            const auto elapsed = ntp_delta_us(report.ntp, mapping->report.ntp);
            const auto media =
                rtp_delta(report.rtp_timestamp, mapping->report.rtp_timestamp) * 1000000 / rate;
            const auto difference = elapsed - media;
            reset = difference < -100000 || difference > 100000;
        }
        if (reset) {
            video.reset();
            audio.reset();
            ++resets;
        }
        if (!origin_ntp) {
            origin_ntp = report.ntp;
        }
        mapping = Mapping{report, rate};
        return reset;
    }

    static Action action(std::int64_t video_us, std::int64_t audio_us) {
        const auto delta = video_us - audio_us;
        if (delta > 10000) {
            return Action::Wait;
        }
        return delta < -80000 ? Action::Drop : Action::Present;
    }

    std::optional<Mapping> video;
    std::optional<Mapping> audio;
    std::optional<std::uint64_t> origin_ntp;
    std::uint64_t resets = 0;
};

}  // namespace rtsp
