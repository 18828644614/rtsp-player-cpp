"""回环假服务器验证真实可执行程序；只用 Python 标准库，不采集设备。"""
import argparse
import re
import socket
import struct
import subprocess
import threading
import time


def frame(channel, payload):
    return b"$" + struct.pack("!BH", channel, len(payload)) + payload


def rtp(pt, source, sequence):
    return struct.pack("!BBHII", 0x80, pt, sequence & 65535,
                       (sequence * 1024) & 0xffffffff, source) + b"payload$\x00"


def rtcp(source, bye=False):
    sr = struct.pack("!BBHIIIIII", 0x80, 200, 6, source, 4, 5, 6, 7, 8)
    sdes = struct.pack("!BBHI", 0x81, 202, 2, source) + b"\x01\x01x\x00"
    return sr + sdes + (struct.pack("!BBHI", 0x81, 203, 1, source) if bye else b"")


class Server:
    def __init__(self, mode):
        self.mode = mode
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.url = f"rtsp://127.0.0.1:{self.listener.getsockname()[1]}/live"
        self.methods = []
        self.error = None
        self.sequence = 65534
        self.setup = 0
        self.playing = False
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def media(self, bye=False):
        self.sequence += 1
        return (frame(6, rtp(104, 123, self.sequence)) + frame(7, rtcp(123, bye))
                + frame(10, rtp(111, 456, self.sequence)) + frame(11, rtcp(456, bye)))

    def respond(self, conn, cseq, headers=None, body=b"", code=200):
        fields = {"CSeq": cseq, "Content-Length": str(len(body))}
        fields.update(headers or {})
        wire = f"RTSP/1.0 {code} Test\r\n".encode()
        wire += b"".join(f"{k}: {v}\r\n".encode() for k, v in fields.items()) + b"\r\n" + body
        if self.mode == "happy":
            # 响应头和 body 都跨发送边界，正文和交织数据里含 '$'。
            for split in (1, 13, len(wire)):
                conn.sendall(wire[:split])
                wire = wire[split:]
                time.sleep(0.002)
        else:
            conn.sendall(wire)

    def handle(self, conn, method, uri, headers):
        self.methods.append(method)
        cseq = headers["cseq"]
        if method == "OPTIONS":
            if self.mode == "timeout":
                return True
            self.respond(conn, cseq)
        elif method == "DESCRIBE":
            assert headers.get("accept") == "application/sdp"
            video_control = "same" if self.mode == "duplicate_url" else "v-track"
            audio_control = self.url + "/same" if self.mode == "duplicate_url" else self.url + "/a-track"
            body = ("v=0\r\ns=contains$dollar\r\na=control:*\r\n"
                    "m=audio 0 RTP/AVP 111\r\na=rtpmap:111 MPEG4-GENERIC/48000/1\r\n"
                    f"a=fmtp:111 config=1188\r\na=control:{audio_control}\r\n"
                    "m=video 0 RTP/AVP 104\r\na=rtpmap:104 H264/90000\r\n"
                    f"a=control:{video_control}\r\n").encode()
            if self.mode == "wrong_cseq":
                cseq = str(int(cseq) + 1)
            self.respond(conn, cseq, {"Content-Type": "application/sdp", "Content-Base": self.url + "/"}, body)
        elif method == "SETUP":
            self.setup += 1
            assert uri == self.url + ("/v-track" if self.setup == 1 else "/a-track")
            assert headers["transport"] == "RTP/AVP/TCP;unicast;interleaved=" + ("0-1" if self.setup == 1 else "2-3")
            if self.setup == 2:
                assert headers.get("session") == "abc"
            channels = "6-7" if self.setup == 1 or self.mode == "overlap" else "10-11"
            sid = "other" if self.mode == "wrong_session" and self.setup == 2 else "abc"
            transport = "RTP/AVP/TCP;unicast;interleaved=" + channels
            if self.mode == "udp_transport":
                transport = "RTP/AVP;unicast;server_port=5000-5001"
            self.respond(conn, cseq, {"Session": sid + ";timeout=1", "Transport": transport})
        elif method == "PLAY":
            assert uri == self.url and headers.get("session") == "abc"
            conn.sendall(self.media())  # PLAY 响应之前到达的媒体也要统计。
            self.respond(conn, cseq, {"Session": "abc"})
            self.playing = True
            if self.mode == "eof":
                return False
            if self.mode == "partial_eof":
                conn.sendall(b"$\x06\x00\x20short")
                return False
            if self.mode == "happy":
                conn.sendall(frame(6, b"bad") + frame(222, b"unknown"))
        elif method == "GET_PARAMETER":
            assert uri == self.url and headers.get("session") == "abc"
            assert headers["content-length"] == "0"
            conn.sendall(self.media())
            if self.mode == "keepalive_timeout":
                return True  # 持续发媒体，控制响应仍必须按期超时。
            self.respond(conn, cseq, {"Session": "abc"}, code=454 if self.mode == "keepalive_error" else 200)
            conn.sendall(self.media())
        elif method == "TEARDOWN":
            assert uri == self.url and headers.get("session") == "abc"
            if self.playing:
                conn.sendall(self.media(bye=True))
            if self.mode == "teardown_timeout":
                while not self.stop.wait(0.03):
                    if self.playing:
                        conn.sendall(self.media())
                return False
            self.respond(conn, cseq, {"Session": "abc"})
            return False
        else:
            raise AssertionError(f"unexpected method {method}")
        return True

    def serve(self):
        try:
            with self.listener:
                conn, _ = self.listener.accept()
                with conn:
                    conn.settimeout(0.03)
                    buf = b""
                    while not self.stop.is_set():
                        try:
                            data = conn.recv(16384)
                            if not data:
                                return
                            buf += data
                        except socket.timeout:
                            if self.playing:
                                conn.sendall(self.media())
                            continue
                        while b"\r\n\r\n" in buf:
                            block, rest = buf.split(b"\r\n\r\n", 1)
                            lines = block.decode().split("\r\n")
                            method, uri, version = lines[0].split()
                            assert version == "RTSP/1.0"
                            headers = {k.lower(): v.strip() for k, v in (line.split(":", 1) for line in lines[1:])}
                            length = int(headers.get("content-length", "0"))
                            if len(rest) < length:
                                break
                            buf = rest[length:]
                            if not self.handle(conn, method, uri, headers):
                                return
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            pass  # 客户端故障退出或限时 TEARDOWN 后关闭是预期行为。
        except BaseException as error:
            self.error = error


def run_case(player, mode, expected=1, seconds=2, timeout=350):
    server = Server(mode)
    server.thread.start()
    started = time.monotonic()
    try:
        result = subprocess.run([player, server.url, "--seconds", str(seconds),
                                 "--timeout-ms", str(timeout)], capture_output=True, text=True, timeout=7)
    finally:
        server.stop.set()
        server.thread.join(2)
    elapsed = time.monotonic() - started
    assert not server.thread.is_alive(), f"{mode}: fake server did not stop"
    if server.error:
        raise AssertionError(f"{mode}: fake server failed: {server.error}")
    output = result.stdout + result.stderr
    assert result.returncode == expected, f"{mode}: exit={result.returncode}\n{output}"
    if mode == "happy":
        assert server.methods[:5] == ["OPTIONS", "DESCRIBE", "SETUP", "SETUP", "PLAY"], output
        assert server.methods.count("GET_PARAMETER") >= 2 and server.methods[-1] == "TEARDOWN", output
        for kind in ("video", "audio"):
            line = re.search(rf"\[FINAL\] {kind} (.+)", output).group(1)
            counts = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            assert counts["rtp"] > 2 and counts["sr"] > 2 and counts["bye"] == 1, output
            assert counts["rtcp_packets"] == 2 * counts["rtcp_frames"] + 1, output
            assert counts["bad"] == (1 if kind == "video" else 0), output
        assert "unknown_channels=1" in output, output
    else:
        required = {"timeout": "OPTIONS response timeout", "wrong_cseq": "wrong CSeq",
                    "wrong_session": "Session id changed", "overlap": "overlapping channels",
                    "udp_transport": "did not negotiate RTP/AVP/TCP",
                    "duplicate_url": "duplicate resolved track URL", "eof": "server disconnected",
                    "partial_eof": "incomplete RTSP", "keepalive_error": "GET_PARAMETER failed: 454",
                    "keepalive_timeout": "GET_PARAMETER response timeout",
                    "teardown_timeout": "TEARDOWN response timeout"}[mode]
        assert required in output, f"{mode}: expected {required}\n{output}"
        if mode == "teardown_timeout":
            assert elapsed < 2.5 and "[FINAL] audio" in output, output
    print(f"PASS: {mode} ({elapsed:.2f}s)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", required=True)
    player = parser.parse_args().player
    run_case(player, "happy", expected=0, timeout=1500)
    for mode in ("timeout", "wrong_cseq", "wrong_session", "overlap", "udp_transport", "duplicate_url",
                 "eof", "partial_eof", "keepalive_error", "keepalive_timeout"):
        run_case(player, mode)
    run_case(player, "teardown_timeout", expected=0, seconds=1, timeout=1500)
    # 不接受缺值、负值和溢出的命令行参数。
    for args in (["bad-url"], ["rtsp://localhost/live", "--seconds"],
                 ["rtsp://localhost/live", "--seconds", "-1"],
                 ["rtsp://localhost/live", "--timeout-ms", "99999999999999"]):
        result = subprocess.run([player, *args], capture_output=True, timeout=3)
        assert result.returncode == 1, args
    print("PASS: command-line errors")


if __name__ == "__main__":
    main()
