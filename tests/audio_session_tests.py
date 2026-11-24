"""真实播放器的双轨解码、AAC 故障恢复、缓冲上限与关闭测试。"""
import argparse
import os
from pathlib import Path
import struct
import subprocess

from player_session_tests import run
from session_tests import frame, rtcp
from video_session_tests import VideoServer, read_audio, read_units, summary


class AudioServer(VideoServer):
    def __init__(self, video, audio, scenario):
        super().__init__(video, "clean")
        self.audio = audio
        self.audio_index = 0
        self.audio_scenario = scenario
        self.teardown = False

    def respond(self, conn, cseq, headers=None, body=b"", code=200):
        if self.audio_scenario == "bad_config":
            body = body.replace(b"config=1188", b"config=1190")
        super().respond(conn, cseq, headers, body, code)

    def handle(self, conn, method, uri, headers):
        if self.audio_scenario == "eof" and method == "GET_PARAMETER":
            return False
        if method == "TEARDOWN":
            self.teardown = True
        return super().handle(conn, method, uri, headers)

    def audio_packet(self):
        index = self.audio_index
        self.audio_index += 1
        data = self.audio[index % len(self.audio)]
        if self.audio_scenario == "loss" and index == 4:
            return b""
        if self.audio_scenario == "corrupt" and index == 4:
            data = b"\0"
        size = len(data)
        payload = b"\0\x10" + struct.pack("!H", size << 3) + data
        if self.audio_scenario == "malformed" and index == 4:
            payload += b"extra"
        timestamp = (0xfffffc00 + index * 1024) & 0xffffffff
        packet = struct.pack("!BBHII", 0x80, 0x80 | 111, (65530 + index) & 65535,
                             timestamp, 456) + payload
        wire = frame(10, packet)
        return wire * 2 if self.audio_scenario == "duplicate" and index == 4 else wire

    def media(self, bye=False):
        count = 40 if self.audio_scenario == "burst" else self.audio_count()
        audio = b"" if bye else b"".join(self.audio_packet() for _ in range(count))
        return self.reports(bye) + (b"" if bye else self.timed_video()) + audio


def check_playback(result, output, scenario):
    assert result.returncode == (1 if scenario == "eof" else 0), output
    audio = summary(output, "audio_decode")
    device = summary(output, "audio_output")
    packets = summary(output, "aac")
    assert audio["frames"] > 5 and audio["samples"] == audio["frames"] * 1024, output
    assert device["samples"] == audio["samples"] and 0 < device["queue_peak_ms"] <= 300, output
    assert summary(output, "decode")["frames"] > 0, output
    if scenario != "burst":
        assert summary(output, "display")["frames"] > 0, output
    assert audio["errors"] == (1 if scenario == "corrupt" else 0), output
    assert packets["invalid"] == (1 if scenario == "malformed" else 0), output
    if scenario == "loss":
        assert packets["lost"] == 1, output
    if scenario == "duplicate":
        assert packets["dropped"] == 1, output
    if scenario == "burst":
        assert device["overflows"] > 0, output
    if scenario != "eof":
        assert "[RTSP] TEARDOWN 200" in output, output
    else:
        assert "server disconnected" in output, output
    print(f"PASS: audio {scenario}, frames={audio['frames']}, peak={device['queue_peak_ms']} ms")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--video", required=True)
    parser.add_argument("--audio", required=True)
    args = parser.parse_args()
    video, audio = read_units(args.video), read_audio(args.audio)
    for scenario in ("clean", "malformed", "corrupt", "loss", "duplicate", "burst", "eof"):
        server = AudioServer(video, audio, scenario)
        result, output, _ = run(server, [args.player, server.url, "--seconds", "1"], max_seconds=5)
        check_playback(result, output, scenario)
    for action in ("event", "stop"):
        server = AudioServer(video, audio, "clean")
        result, output, elapsed = run(server, [args.driver, server.url, "350", action])
        assert result.returncode == 0 and elapsed < 1.5 and server.teardown, output
        assert summary(output, "audio_output")["samples"] > 0, output
        print(f"PASS: close during audio playback via {action}, {elapsed:.2f}s")
    server = AudioServer(video, audio, "bad_config")
    result, output, _ = run(server, [args.player, server.url, "--seconds", "1"])
    assert result.returncode == 1 and "AAC config must describe" in output, output
    assert not server.playing, output
    print("PASS: incompatible AAC configuration rejected before SETUP/PLAY")
    failed = subprocess.run([args.player, "rtsp://127.0.0.1:1/live"],
                            env=dict(os.environ, SDL_AUDIODRIVER="missing-test-driver"),
                            capture_output=True, text=True, timeout=3)
    assert failed.returncode == 1 and "initialize SDL audio" in failed.stderr, failed.stderr
    print("PASS: SDL audio initialization failure cleans up before network starts")


if __name__ == "__main__":
    main()
