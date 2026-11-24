"""P5：真实双轨解码、120 ms 偏移、延迟/缺失 SR、重缓冲和音频结束。"""
import argparse
from pathlib import Path
import struct

from player_session_tests import run
from session_tests import frame, rtcp
from video_session_tests import VideoServer, read_units, summary


def ntp(milliseconds):
    return (1000 << 32) + (milliseconds << 32) // 1000


class SyncServer(VideoServer):
    def __init__(self, units, scenario):
        super().__init__(units, "clean")
        self.sync_scenario = scenario
        self.injected = False

    def reports(self, bye=False):
        elapsed = self.elapsed()
        # RTP 起点都接近回绕，音频首样本比视频首帧晚 120 ms。
        milliseconds = int(elapsed * 1000)
        jump = self.sync_scenario == "jump" and elapsed >= 0.5
        report_ntp = ntp(milliseconds + (1000 if jump else 0))
        video = frame(7, rtcp(123, bye, ntp=report_ntp,
                             timestamp=0xffffe000 + milliseconds * 90))
        audio = frame(11, rtcp(456, bye, ntp=report_ntp,
                              timestamp=0xfffffc00 + milliseconds * 48 - 5760))
        if self.sync_scenario == "missing_sr" or (
                self.sync_scenario == "delayed_sr" and elapsed < 0.25):
            audio = b""
        if self.sync_scenario == "bad_rtcp" and not self.injected:
            self.injected = True
            # 前面的 SR 合法，但复合尾包损坏；整条 RTCP 必须拒绝。
            video = frame(7, rtcp(123, ntp=report_ntp, timestamp=0xffffe000) + b"bad")
        return video + audio

    def media(self, bye=False):
        video = b"" if bye else self.timed_video()
        silent = self.sync_scenario == "audio_end" and self.elapsed() >= 0.65
        audio = b"" if bye or silent else b"".join(
            self.audio_packet() for _ in range(self.audio_count()))
        return self.reports(bye) + video + audio


def test_playback(player, units, scenario):
    server = SyncServer(units, scenario)
    seconds = "6" if scenario == "missing_sr" else "2"
    result, output, elapsed = run(server, [player, server.url, "--seconds", seconds], max_seconds=6)
    if scenario == "missing_sr":
        assert result.returncode == 1 and "waiting for both RTCP SR" in output, output
        assert 2.8 <= elapsed < 4.5, (elapsed, output)
        assert summary(output, "audio_output")["samples"] == 0, output
        assert summary(output, "display")["frames"] == 0, output
        assert "[RTSP] TEARDOWN 200" in output, output
    else:
        assert result.returncode == 0, output
        sync = summary(output, "sync")
        display = summary(output, "display")
        decode = summary(output, "decode")
        assert sync["ready"] == 1 and display["frames"] >= 3, output
        assert 0 <= sync["max_present_error_us"] <= 80000, output
        assert display["frames"] + display["dropped"] == decode["frames"], output
        assert summary(output, "audio_output")["queue_peak_ms"] <= 300, output
        assert summary(output, "video")["bad"] == (1 if scenario == "bad_rtcp" else 0), output
        assert "[RTSP] TEARDOWN 200" in output, output
        if scenario == "delayed_sr":
            assert sync["waiting_audio_samples"] > 0, output
        if scenario == "jump":
            assert sync["resets"] == 1 and output.count("[SYNC] ready") == 2, output
        else:
            assert sync["resets"] == 0, output
        if scenario == "audio_end":
            assert sync["tail_dropped"] > 0, output
    print(f"PASS: sync {scenario}, elapsed={elapsed:.2f}s")


def test_report_callbacks(driver, units):
    # 使用协议层测试入口，确保普通 RTCP 不重放 latest_sr，异常不被当成坏包吞掉。
    server = SyncServer(units, "clean")
    result, output, _ = run(server, [driver, server.url, "0", "sr-throw"])
    assert result.returncode == 1 and "test SR handler failure" in output, output
    assert summary(output, "video")["bad"] == 0, output
    assert "[RTSP] TEARDOWN 200" in output, output

    class ReportsOnly(VideoServer):
        def media(self, bye=False):
            if bye:
                return b""
            if not self.injected:
                self.injected = True
                return self.reports()
            if not hasattr(self, "bad_sent"):
                self.bad_sent = True
                return frame(7, rtcp(123) + b"bad")
            # 无 SR 的 Receiver Report；不能再次回调缓存的 latest_sr。
            return frame(7, struct.pack("!BBHI", 0x80, 201, 1, 123))

    server = ReportsOnly(units, "clean")
    result, output, _ = run(server, [driver, server.url, "0", "sr-count"])
    assert result.returncode == 0 and "[TEST] reports=2" in output, output
    assert summary(output, "video")["bad"] == 1, output
    print("PASS: SR callback only for current validated reports; exceptions propagate")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    units = read_units(Path(args.fixture))
    for scenario in ("clean", "delayed_sr", "missing_sr", "jump", "bad_rtcp", "audio_end"):
        test_playback(args.player, units, scenario)
    test_report_callbacks(args.driver, units)


if __name__ == "__main__":
    main()
