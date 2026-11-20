"""启动配套服务端的 --serve 合成入口，验收播放器的实际双轨收包。"""
import argparse
from pathlib import Path
import queue
import re
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, help="camera_rtsp_tests.exe path")
    parser.add_argument("--player", required=True, help="rtsp_player.exe path")
    parser.add_argument("--log", help="optional UTF-8 player output log")
    args = parser.parse_args()
    server_path = Path(args.server).resolve(strict=True)
    player_path = Path(args.player).resolve(strict=True)
    lines = queue.Queue()
    diagnostics = []
    with subprocess.Popen([str(server_path), "--serve"], cwd=server_path.parent,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace") as server:
        def drain():
            for line in server.stdout:
                diagnostics.append(line)
                if "live_test_url=" in line:
                    lines.put(line.split("live_test_url=", 1)[1].strip())
            lines.put(None)
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        try:
            try:
                url = lines.get(timeout=8)
            except queue.Empty:
                raise AssertionError("server did not print URL: " + "".join(diagnostics)) from None
            if url is None:
                raise AssertionError("server exited before URL: " + "".join(diagnostics))
            result = subprocess.run([str(player_path), url, "--seconds", "4"],
                                    capture_output=True, text=True, timeout=9)
            output = result.stdout + result.stderr
            if args.log:
                Path(args.log).write_text(output, encoding="utf-8")
            print(output, end="")
            assert result.returncode == 0, f"player exit={result.returncode}"
            for kind in ("video", "audio"):
                match = re.search(rf"\[FINAL\] {kind} (.+)", output)
                assert match, f"missing {kind} statistics"
                counts = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match.group(1))}
                assert counts["rtp"] > 0 and counts["sr"] > 0 and counts["bad"] == 0, match.group(0)
            assert "[RTSP] TEARDOWN 200" in output and "unknown_channels=0" in output
            print("PASS: companion server, dual RTP/RTCP, clean TEARDOWN")
        finally:
            # 让短时入口自行结束，验证测试服务端也能正常收尾。
            try:
                server.wait(timeout=14)
            except subprocess.TimeoutExpired:
                server.terminate()
                server.wait(timeout=3)
            reader.join(timeout=2)
        assert server.returncode == 0, "server failed: " + "".join(diagnostics)


if __name__ == "__main__":
    main()
