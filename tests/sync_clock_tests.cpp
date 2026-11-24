#include "player/sync_clock.h"

#include <iostream>
#include <stdexcept>

namespace {

using Clock = rtsp::SyncClock;
using Track = Clock::Track;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::uint64_t ntp(std::uint32_t seconds, std::uint32_t milliseconds = 0) {
    return (static_cast<std::uint64_t>(seconds) << 32)
           + (static_cast<std::uint64_t>(milliseconds) << 32) / 1000;
}

bool close(std::int64_t a, std::int64_t b) {
    return a >= b - 2 && a <= b + 2;
}

void test_alignment(bool audio_first) {
    Clock clock;
    const std::uint32_t video_origin = 0xffffe000U;
    const std::uint32_t audio_origin = 0xfffffc00U;
    const rtsp::SenderReport video{123, ntp(1000, 500), video_origin + 45000U};
    const rtsp::SenderReport audio{456, ntp(1000, 750), audio_origin + 30240U};
    check(!clock.time(Track::Video, video_origin), "missing SR produced a video time");
    if (audio_first) {
        clock.update(Track::Audio, 48000, audio);
        check(!clock.ready(), "one SR established sync");
        clock.update(Track::Video, 90000, video);
    } else {
        clock.update(Track::Video, 90000, video);
        check(!clock.ready(), "one SR established sync");
        clock.update(Track::Audio, 48000, audio);
    }
    check(clock.ready(), "two SRs did not establish sync");
    const auto video_start = *clock.time(Track::Video, video_origin);
    const auto audio_start = *clock.time(Track::Audio, audio_origin);
    check(close(audio_start - video_start, 120000), "120 ms track offset was lost");
    check(
        close(
            *clock.time(Track::Video, static_cast<std::int64_t>(video_origin) + 10800), audio_start
        ),
        "matching media instants differ after RTP wrap"
    );
    check(
        close(
            *clock.time(Track::Audio, static_cast<std::int64_t>(audio_origin) + 48000),
            audio_start + 1000000
        ),
        "audio RTP wrap changed elapsed time"
    );

    // SR 更新的时刻各不相同，但不能改变已有帧在公共轴上的位置。
    clock.update(Track::Video, 90000, {123, ntp(1001, 500), video_origin + 135000U});
    clock.update(Track::Audio, 48000, {456, ntp(1001, 750), audio_origin + 78240U});
    check(
        close(*clock.time(Track::Video, video_origin), video_start), "video SR update moved time"
    );
    check(
        close(*clock.time(Track::Audio, audio_origin), audio_start), "audio SR update moved time"
    );
    check(clock.resets == 0, "consistent reports reset sync");
    clock.update(Track::Video, 90000, {123, ntp(999), 12345});
    clock.update(Track::Video, 90000, {123, ntp(1001, 500), 12345});
    check(
        close(*clock.time(Track::Video, video_origin), video_start), "old/duplicate SR moved time"
    );
}

void test_discontinuity() {
    Clock clock;
    clock.update(Track::Video, 90000, {123, ntp(1000), 0});
    clock.update(Track::Audio, 48000, {456, ntp(1000), 0});
    check(
        clock.update(Track::Video, 90000, {123, ntp(1001), 180000}),
        "one-second mapping jump was not detected"
    );
    check(!clock.ready() && clock.resets == 1 && !clock.audio, "old audio mapping survived reset");
    clock.update(Track::Audio, 48000, {456, ntp(1001), 96000});
    check(clock.ready(), "new audio SR did not restore sync");
    bool rejected = false;
    try {
        clock.update(Track::Audio, 44100, {456, ntp(1002), 144000});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "unsupported time base accepted");
}

void test_schedule() {
    using Action = Clock::Action;
    check(Clock::action(120000, 0) == Action::Wait, "early video should wait");
    check(Clock::action(120000, 110000) == Action::Present, "early tolerance changed");
    check(Clock::action(120000, 120000) == Action::Present, "aligned video should display");
    check(Clock::action(120000, 200000) == Action::Present, "late tolerance changed");
    check(Clock::action(120000, 200001) == Action::Drop, "late video should be dropped");
}

}  // namespace

int main() {
    try {
        test_alignment(false);
        test_alignment(true);
        test_discontinuity();
        test_schedule();
        std::cout << "PASS: SR alignment, 120 ms offset, RTP wrap, updates and video scheduling\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
