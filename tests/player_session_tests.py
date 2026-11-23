"""P3 回环测试：窗口/外部停止、保活等待、TEARDOWN 超时、有界队列与断线。"""
import argparse
import os
import subprocess
import time

from session_tests import Server
from video_session_tests import VideoServer, read_units, summary


class ClosingServer(Server):
    def __init__(self, mode):
        super().__init__(mode)
        self.teardown = False
        self.keepalive = False
        self.options = False

    def handle(self, conn, method, uri, headers):
        if method == "OPTIONS":
            self.options = True
        if method == "GET_PARAMETER":
            self.keepalive = True
        if method == "TEARDOWN":
            self.teardown = True
        return super().handle(conn, method, uri, headers)


def run(server, command, max_seconds=4):
    server.thread.start()
    started = time.monotonic()
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=max_seconds)
    finally:
        server.stop.set()
        server.thread.join(2)
    assert not server.thread.is_alive(), "server failed to stop"
    assert server.error is None, server.error
    return result, result.stdout + result.stderr, time.monotonic() - started


def test_close(driver, mode, delay, action):
    server = ClosingServer(mode)
    result, output, elapsed = run(server, [driver, server.url, str(delay), action])
    assert result.returncode == 0, output
    assert elapsed < delay / 1000 + 1.5, (elapsed, output)
    assert "[FINAL] display" in output, output
    if mode == "timeout":
        assert server.options and not server.playing, output
    else:
        assert server.playing and server.teardown, output
    if mode == "keepalive_timeout":
        assert server.keepalive, output
    if mode == "teardown_timeout":
        assert "TEARDOWN response timeout" in output, output
    elif mode != "timeout":
        assert "[RTSP] TEARDOWN 200" in output, output
    print(f"PASS: close {action}, {mode}, {elapsed:.2f}s")


def test_video(player, units, scenario):
    server = VideoServer(units, scenario)
    result, output, _ = run(server, [player, server.url, "--seconds", "2"], max_seconds=6)
    assert result.returncode == (1 if scenario == "eof" else 0), output
    display = summary(output, "display")
    decode = summary(output, "decode")
    assert 0 < display["frames"] <= decode["frames"], output
    assert 1 <= display["queue_peak"] <= 12, output
    assert display["dropped"] + display["frames"] == decode["frames"], output
    if scenario == "clean":
        assert display["dropped"] > 0, "fast fixture should exercise overflow/late-frame drops"
        assert display["clock_resets"] == 0, output
    else:
        assert "server disconnected" in output, output
    print(f"PASS: {scenario}, displayed={display['frames']}, dropped={display['dropped']}, peak={display['queue_peak']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    for mode, delay, action in (("timeout", 200, "event"), ("video", 250, "event"),
                                ("video", 250, "stop"), ("keepalive_timeout", 800, "event"),
                                ("teardown_timeout", 250, "event")):
        test_close(args.driver, mode, delay, action)
    units = read_units(args.fixture)
    server = VideoServer(units, "clean")
    result, output, _ = run(server, [args.driver, server.url, "500", "event"])
    assert result.returncode == 0 and "[RTSP] TEARDOWN 200" in output, output
    assert summary(output, "decode")["frames"] > 0 and summary(output, "display")["frames"] > 0, output
    assert summary(output, "display")["frames"] + summary(output, "display")["dropped"] == summary(output, "decode")["frames"], output
    print("PASS: close window during real H264 playback")
    bad_environment = dict(os.environ, SDL_RENDER_DRIVER="missing-test-renderer")
    failed = subprocess.run([args.player, "rtsp://127.0.0.1:1/live"], env=bad_environment,
                            capture_output=True, text=True, timeout=3)
    assert failed.returncode == 1 and "create video renderer" in failed.stderr, failed.stderr
    print("PASS: SDL initialization failure cleans up before network starts")
    for scenario in ("clean", "eof"):
        test_video(args.player, units, scenario)
    print("PASS: P3 player lifecycle and bounded playback")


if __name__ == "__main__":
    main()
