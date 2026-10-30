#!/usr/bin/env python3
"""CPU integration: burn-ins go into the alpha. Reads the MXL grains of a v210a output and of a
fill_key output back (build/player_grain_dump) and checks alpha and key values under a moving box
and a text box. `--shots DIR` also writes the fill and the key of each case as PNG."""

import json
import os
import signal
import struct
import subprocess
import sys
import time
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "mxl-test-player"
DUMP = ROOT / "build" / "player_grain_dump"
BASE = os.environ.get("PLAYER_BASE", "http://127.0.0.1:18430")
PORT = int(BASE.rsplit(":", 1)[-1])
DOMAIN = "/dev/shm/mtp-keying"
W, H = 1280, 720
STRIDE = ((W + 47) // 48) * 128
ASTRIDE = ((W + 2) // 3) * 4


def http(method, path, body=None, raw=None, timeout=60):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    headers = {"Content-Type": "application/json"} if body is not None else {}
    req = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as res:
        raw_body = res.read()
        if raw_body and res.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw_body)
        return raw_body


def grain(flow_id, work):
    """Payload of a grain written a few grains ago."""
    out = work / "grain.bin"
    subprocess.check_call([str(DUMP), DOMAIN, flow_id, "-3", str(out)], stdout=subprocess.DEVNULL)
    return out.read_bytes()


def words(buf, off, n):
    return struct.unpack_from(f"<{n}I", buf, off)


def luma(buf, x, y):
    g, i = divmod(x, 6)
    w = words(buf, y * STRIDE + g * 16, 4)
    return [w[0] >> 10, w[1], w[1] >> 20, w[2] >> 10, w[3], w[3] >> 20][i] & 0x3FF


def alpha(buf, x, y):
    w = words(buf, STRIDE * H + y * ASTRIDE + (x // 3) * 4, 1)[0]
    return (w >> ((x % 3) * 10)) & 0x3FF


def rgb(buf, x, y):
    g, i = divmod(x, 6)
    w = words(buf, y * STRIDE + g * 16, 4)
    ys = [w[0] >> 10, w[1], w[1] >> 20, w[2] >> 10, w[3], w[3] >> 20]
    cbs = [w[0], w[1] >> 10, w[2] >> 20]
    crs = [w[0] >> 20, w[2], w[3] >> 10]
    Y = ((ys[i] & 0x3FF) - 64) / 876
    cb = ((cbs[i // 2] & 0x3FF) - 512) / 896
    cr = ((crs[i // 2] & 0x3FF) - 512) / 896
    px = (Y + 1.5748 * cr, Y - 0.1873 * cb - 0.4681 * cr, Y + 1.8556 * cb)
    return bytes(max(0, min(255, int(v * 255 + 0.5))) for v in px)


def png(path, w, h, rows):
    raw = b"".join(b"\0" + r for r in rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def shots(shot_dir, name, fill, key_of):
    """Half-size PNGs of the fill and of the key (alpha as grey)."""
    if not shot_dir:
        return
    fr, kr = [], []
    for y in range(0, H, 2):
        fr.append(b"".join(rgb(fill, x, y) for x in range(0, W, 2)))
        kr.append(b"".join(bytes([min(255, key_of(x, y))] * 3) for x in range(0, W, 2)))
    png(shot_dir / f"tp130-{name}-fill.png", W // 2, H // 2, fr)
    png(shot_dir / f"tp130-{name}-key.png", W // 2, H // 2, kr)


def main():
    shot_dir = None
    if "--shots" in sys.argv:
        shot_dir = Path(sys.argv[sys.argv.index("--shots") + 1])
        shot_dir.mkdir(parents=True, exist_ok=True)
    work = Path("/tmp/mtp-keying")
    work.mkdir(parents=True, exist_ok=True)
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
            "MXL_OUTPUT_DOMAIN_DIR": DOMAIN,
            "MXL_CLEANUP_ON_EXIT": "true",
            "NMOS_SEED": "keying-player",
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
                http("GET", "/livez")
                break
            except Exception:
                time.sleep(0.1)
        else:
            raise SystemExit("player did not start")

        # A transparent background (bars with the idle key transparent), no standard burn-ins,
        # a built-in moving box at 60 % parked top left, and a text layer with a 50 % box bottom right.
        burn = {
            "label": False, "timecode": False, "frame_counter": False, "item": False, "flow_id": False,
            "texts": [{"text": "KEY", "anchor": "br", "size": 8, "color": "#FFFFFF", "opacity": 1, "box": True,
                       "box_color": "#000000", "box_opacity": 0.5, "box_padding": 0.5}],
            "boxes": [{"enabled": True, "content": "builtin", "path": "horizontal", "speed": 0, "size": 0.15,
                       "opacity": 0.6, "color": "#FFCC00", "start_x": 0, "start_y": 0}],
        }
        http("PATCH", "/api/v1/outputs/0", {"key_mode": "v210a", "idle_key": "transparent"})
        http("PUT", "/api/v1/outputs/0/source", {"type": "pattern", "pattern": "smpte_rp219", "audio": "silence"})
        http("PUT", "/api/v1/outputs/0/burnin", burn)
        time.sleep(0.6)
        st = http("GET", "/api/v1/outputs/0")
        g = grain(st["video_flow_id"], work)
        assert len(g) == STRIDE * H + ASTRIDE * H, len(g)
        box_a = alpha(g, 60, 20)
        text_a = alpha(g, W - 3, H - 3)
        bg_a = alpha(g, W // 2, H // 2)
        assert abs(box_a - round(0.6 * 1023)) <= 1, box_a
        assert abs(text_a - round(127 / 255 * 1023)) <= 1, text_a
        assert bg_a == 0, bg_a
        shots(shot_dir, "v210a-burnin", g, lambda x, y: alpha(g, x, y) >> 2)

        # The same in fill_key: the key flow carries the burn-ins at the key range 64..940.
        http("PATCH", "/api/v1/outputs/0", {"key_mode": "fill_key"})
        time.sleep(0.6)
        st = http("GET", "/api/v1/outputs/0")
        fill = grain(st["video_flow_id"], work)
        key = grain(st["key_flow_id"], work)
        kbox = luma(key, 60, 20)
        ktext = luma(key, W - 3, H - 3)
        kbg = luma(key, W // 2, H // 2)
        assert abs(kbox - (64 + round(0.6 * 876))) <= 1, kbox
        assert abs(ktext - (64 + round(127 / 255 * 876))) <= 1, ktext
        assert kbg == 64, kbg
        shots(shot_dir, "fillkey-burnin", fill, lambda x, y: max(0, luma(key, x, y) - 64) * 255 // 876)
        assert http("GET", "/api/v1/outputs/0")["underruns"] == 0
        print("keying ok", "v210a box/text/bg", box_a, text_a, bg_a, "key box/text/bg", kbox, ktext, kbg)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
