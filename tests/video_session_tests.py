"""用固定合成码流验证 RTP → AU → FFmpeg，包括缺片、坏包和码流损坏后的恢复。"""
import argparse
import base64
from pathlib import Path
import re
import struct
import subprocess

from session_tests import Server, frame, rtp, rtcp


PRESENTATION_ORDER = (0, 3, 1, 2, 4, 7, 5, 6)


def read_units(path):
    wire = Path(path).read_bytes()
    starts = list(re.finditer(b"\x00\x00\x00?\x01", wire))
    units = []
    current = []
    for index, start in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(wire)
        nalu = wire[start.end():end]
        if nalu[0] & 31 == 9 and current:
            units.append(current)
            current = []
        current.append(nalu)
    if current:
        units.append(current)
    assert len(units) == 8
    return units


def packetize(nalu, limit=16):
    if len(nalu) <= limit:
        return [nalu]
    pieces = [nalu[i:i + limit - 2] for i in range(1, len(nalu), limit - 2)]
    payloads = []
    for index, piece in enumerate(pieces):
        flags = (0x80 if index == 0 else 0) | (0x40 if index + 1 == len(pieces) else 0)
        payloads.append(bytes(((nalu[0] & 0xe0) | 28, (nalu[0] & 31) | flags)) + piece)
    return payloads


class VideoServer(Server):
    def __init__(self, units, scenario):
        super().__init__("video")
        self.units = units
        self.scenario = scenario
        self.video_index = 0
        self.video_sequence = 65530
        self.injected = False
        sps = next(nalu for nalu in units[0] if nalu[0] & 31 == 7)
        pps = next(nalu for nalu in units[0] if nalu[0] & 31 == 8)
        self.parameters = ",".join(base64.b64encode(nalu).decode() for nalu in (sps, pps))

    def respond(self, conn, cseq, headers=None, body=b"", code=200):
        if body:
            body = body.replace(b"a=rtpmap:104 H264/90000\r\n",
                                ("a=rtpmap:104 H264/90000\r\n"
                                 "a=fmtp:104 packetization-mode=1;sprop-parameter-sets="
                                 + self.parameters + "\r\n").encode())
        super().respond(conn, cseq, headers, body, code)

    def handle(self, conn, method, uri, headers):
        if self.scenario == "eof" and method == "GET_PARAMETER" and self.video_index == 24:
            return False
        return super().handle(conn, method, uri, headers)

    def video(self):
        if self.video_index == 24:
            return b""
        index = self.video_index
        self.video_index += 1
        nalus = self.units[index % 8]
        if self.scenario == "corrupt" and index == 9:
            nalus = [b"\x65\x80"]  # RTP/AU 完整，但 slice 码流无效。
            self.injected = True
        payloads = [payload for nalu in nalus for payload in packetize(nalu)]
        timestamp = (0xffffe000 + (index // 8 * 8 + PRESENTATION_ORDER[index % 8]) * 9000) & 0xffffffff
        output = bytearray()
        for part, payload in enumerate(payloads):
            self.video_sequence = (self.video_sequence + 1) & 65535
            if (self.scenario == "loss" and index == 9 and payload[0] & 31 == 28
                    and payload[1] & 0xc0 == 0 and not self.injected):
                self.injected = True
                continue  # 保留序号缺口，模拟 FU-A 中间片丢失。
            marker = 0x80 if part + 1 == len(payloads) else 0
            version = 0x80
            if self.scenario == "bad_rtp" and index == 9 and part == 0:
                version = 0x40
                self.injected = True
            packet = struct.pack("!BBHII", version, 104 | marker,
                                 self.video_sequence, timestamp, 123) + payload
            output.extend(frame(6, packet))
        return bytes(output)

    def media(self, bye=False):
        self.sequence += 1
        video = b"" if bye else self.video()
        return (video + frame(7, rtcp(123, bye))
                + frame(10, rtp(111, 456, self.sequence)) + frame(11, rtcp(456, bye)))


def summary(output, kind):
    match = re.search(rf"\[FINAL\] {kind} (.+)", output)
    assert match, f"missing {kind} summary\n{output}"
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match[1])}


def run_case(player, units, scenario):
    server = VideoServer(units, scenario)
    server.thread.start()
    try:
        result = subprocess.run([player, server.url, "--seconds", "2", "--timeout-ms", "1000"],
                                capture_output=True, text=True, timeout=6)
    finally:
        server.stop.set()
        server.thread.join(2)
    assert not server.thread.is_alive(), "fake server did not stop"
    assert server.error is None, server.error
    output = result.stdout + result.stderr
    expected_exit = 1 if scenario == "eof" else 0
    assert result.returncode == expected_exit, output
    frames = re.findall(r"\[VIDEO\] frame=(\d+) pts=(-?\d+) width=(\d+) height=(\d+) format=(\w+)", output)
    assert frames and all(w == "64" and h == "48" and fmt == "yuv420p"
                          for _, _, w, h, fmt in frames), output
    timestamps = [int(pts) for _, pts, _, _, _ in frames]
    assert all(a < b for a, b in zip(timestamps, timestamps[1:])), output
    assert 72000 in timestamps and timestamps[-1] == 171000, f"IDR recovery/final drain failed\n{output}"
    decode = summary(output, "decode")
    h264 = summary(output, "h264")
    assert decode["frames"] == len(frames) and h264["au"] >= len(frames), output
    assert summary(output, "video")["bad"] == (1 if scenario == "bad_rtp" else 0), output
    if scenario in ("clean", "eof"):
        assert len(frames) == 20 and decode["errors"] == 0 and h264["invalid"] == 0, output
    else:
        assert server.injected and len(frames) < 20 and decode["resets"] >= 2, output
        assert (decode["errors"] > 0) == (scenario == "corrupt"), output
    if scenario == "eof":
        assert "server disconnected" in output, output
    else:
        assert "[RTSP] TEARDOWN 200" in output, output
    print(f"PASS: {scenario}, frames={len(frames)}, decoder_errors={decode['errors']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    units = read_units(args.fixture)
    for scenario in ("clean", "loss", "bad_rtp", "corrupt", "eof"):
        run_case(args.player, units, scenario)


if __name__ == "__main__":
    main()
