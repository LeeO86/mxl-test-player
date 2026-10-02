#!/usr/bin/env python3
"""CPU integration: convert a short interlaced clip, loop it, and check MXL identity."""

import json
import os
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "mxl-test-player"
BASE = os.environ.get("PLAYER_BASE", "http://127.0.0.1:18130")
PORT = int(BASE.rsplit(":", 1)[-1])


def http(method, path, body=None, raw=None, timeout=60):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    headers = {}
    if body is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as res:
        raw_body = res.read()
        if not raw_body:
            return None
        if res.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw_body)
        return raw_body


def wait_job(item_id, timeout=90):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        item = http("GET", f"/api/v1/library/{item_id}")
        last = item["conversions"]
        states = [v.get("status") for v in last.values()]
        if states and all(s in ("ready", "failed") for s in states):
            if any(s == "failed" for s in states):
                raise SystemExit(f"conversion failed: {last}")
            return item
        time.sleep(0.3)
    raise SystemExit(f"conversion timeout: {last}")


def main():
    work = Path("/tmp/mtp-int")
    work.mkdir(parents=True, exist_ok=True)
    clip = work / "clip.mp4"
    png = work / "key.png"
    gif = work / "box.gif"
    subprocess.check_call(
        [
            "ffmpeg", "-hide_banner", "-y",
            "-f", "lavfi", "-i", "testsrc=size=640x360:rate=25:duration=0.4",
            "-f", "lavfi", "-i", "sine=frequency=1000:duration=0.4:sample_rate=48000",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "1", "-flags", "+ildct+ilme", "-field_order", "tt",
            "-c:a", "aac", "-ac", "2", str(clip),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.check_call(
        [
            "ffmpeg", "-hide_banner", "-y", "-f", "lavfi", "-i", "color=c=red:s=32x32,format=rgba",
            "-vf", "geq=r=255:g=0:b=0:a='if(lt(X,16),0,255)'", "-frames:v", "1", "-update", "1", str(png),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.check_call(
        [
            "ffmpeg", "-hide_banner", "-y", "-f", "lavfi", "-i", "testsrc=size=32x32:rate=10:duration=0.4",
            "-loop", "0", str(gif),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    env = os.environ.copy()
    env.update(
        {
            "PLAYER_FORMAT": "720p25",
            "PLAYER_OUTPUTS": "1",
            "PLAYER_AUDIO_CHANNELS": "2",
            "PLAYER_LIBRARY_DIR": str(work / "lib"),
            "PLAYER_STATE": str(work / "state.json"),
            "PLAYER_FONT_DIR": str(ROOT / "assets" / "fonts"),
            "PLAYER_WEB_ROOT": str(ROOT / "web" / "dist"),
            "MXL_OUTPUT_DOMAIN_DIR": "/dev/shm/mtp-int",
            "NMOS_SEED": "int-player",
            "WEB_PORT": str(PORT),
            "NMOS_PORT": str(PORT + 1),
        }
    )
    log = open(work / "player.log", "w")
    proc = subprocess.Popen([str(BIN), "--config", str(work / "missing.json")], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                assert http("GET", "/livez") == b"ok\n"
                break
            except Exception:
                time.sleep(0.1)
        else:
            raise SystemExit("player did not become ready")

        def upload(path: Path):
            meta = http("POST", "/api/v1/uploads", {"name": path.name, "size": path.stat().st_size, "options": {"fit": "fit"}})
            blob = path.read_bytes()
            http("PUT", f"/api/v1/uploads/{meta['id']}/chunks/0", raw=blob)
            return http("POST", f"/api/v1/uploads/{meta['id']}/complete", {})

        item = upload(clip)
        item = wait_job(item["id"])
        conv = item["conversions"]["720p25"]
        assert conv["status"] == "ready", conv
        assert conv["frames"] >= 1
        assert conv["audio_samples"] == conv["frames"] * 1920 or conv["audio_samples"] > 0
        # 720p25 cadence is 1920 samples/frame.
        assert conv["audio_samples"] == conv["frames"] * 1920, conv

        http("PUT", "/api/v1/outputs/0/source", {"type": "video", "item_id": item["id"]})
        time.sleep(0.8)  # RAM load happens on the writer; ignore that hitch
        http("POST", "/api/v1/outputs/0/transport", {"action": "restart"})
        settled = http("GET", "/api/v1/outputs/0")
        time.sleep(1.4)  # 3 loops of 0.4 s plus margin
        st = http("GET", "/api/v1/outputs/0")
        assert st["underruns"] == settled["underruns"], st
        assert st["loops"] >= 2, st
        assert st["grains"] > settled["grains"] + 20, st
        flow = st["video_flow_id"]

        http("PUT", "/api/v1/outputs/0/source", {"preset": "av_sync"})
        st2 = http("GET", "/api/v1/outputs/0")
        assert st2["video_flow_id"] == flow
        assert st2["source"]["pattern"] == "av_sync"

        burn = st2["burnin"]
        burn["texts"] = [{"text": "LIVE {frame}", "anchor": "mc", "size": 6, "font": "DejaVu Sans", "color": "#FFFFFF", "opacity": 1, "box": True, "box_color": "#000000", "box_opacity": 0.5, "box_padding": 0.3, "outline": False, "shadow": False, "offset_x": 0, "offset_y": 0}]
        burn["boxes"] = [{"enabled": True, "content": "builtin", "path": "bounce", "speed": 0.2, "size": 0.15, "opacity": 1, "color": "#FFCC00", "include_in_key": False, "start_x": 0, "start_y": 0}]
        before = http("GET", "/api/v1/outputs/0")
        g0 = before["grains"]
        u0 = before["underruns"]
        http("PUT", "/api/v1/outputs/0/burnin", burn)
        time.sleep(0.4)
        st3 = http("GET", "/api/v1/outputs/0")
        assert st3["video_flow_id"] == flow
        assert st3["grains"] >= g0 + 8, (g0, st3["grains"])
        assert st3["underruns"] == u0 == 0, (u0, st3["underruns"])

        http("PATCH", "/api/v1/outputs/0", {"format": "1080p25"})
        time.sleep(0.4)
        st4 = http("GET", "/api/v1/outputs/0")
        assert st4["video_flow_id"] != flow
        assert st4["format"] == "1080p25"

        # ANC timecode advances with the grain index. The new flow only has
        # grains written after it was created, so retry a few recent indices.
        import ctypes
        libc = ctypes.CDLL(None)

        class Ts(ctypes.Structure):
            _fields_ = [("sec", ctypes.c_long), ("nsec", ctypes.c_long)]

        t = Ts()
        libc.clock_gettime(11, ctypes.byref(t))
        idx = (t.sec * 10**9 + t.nsec) * 25 // 10**9
        a = b = None
        for back in range(2, 12):
            a = http("GET", f"/api/v1/outputs/0/probe?index={idx - back - 1}")
            b = http("GET", f"/api/v1/outputs/0/probe?index={idx - back}")
            if a["ok"] and b["ok"] and a["anc_ok"] and b["anc_ok"]:
                break
        assert a["ok"] and b["ok"] and a["anc_ok"] and b["anc_ok"], (a, b)
        assert a["anc_timecode"] != b["anc_timecode"]

        still = upload(png)
        still = wait_job(still["id"])
        assert still["type"] == "still"
        http("PATCH", "/api/v1/outputs/0", {"format": "720p25", "key_mode": "fill_key"})
        http("PUT", "/api/v1/outputs/0/source", {"type": "still", "item_id": still["id"], "audio": "silence"})
        time.sleep(0.4)
        st5 = http("GET", "/api/v1/outputs/0")
        assert st5["key_mode"] == "fill_key"
        assert st5["key_flow_id"]
        t = Ts()
        libc.clock_gettime(11, ctypes.byref(t))
        idx = (t.sec * 10**9 + t.nsec) * 25 // 10**9
        key = http("GET", f"/api/v1/outputs/0/probe?index={idx - 2}")
        # Probe reads the fill flow. Key flow is a separate sender; grain index stays in lockstep
        # because both writers share the same loop. Fill is non-black (red).
        assert key["ok"], key
        assert key["y"] > 64

        sprite = upload(gif)
        sprite = wait_job(sprite["id"])
        assert sprite["type"] == "sprite", sprite["type"]

        metrics = http("GET", "/metrics")
        assert b"mxl_test_player_underruns_total" in metrics
        assert b"mxl_test_player_grains_written_total" in metrics
        print("integration ok", "frames", conv["frames"], "samples", conv["audio_samples"], "loops", st["loops"])
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
