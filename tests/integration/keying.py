#!/usr/bin/env python3
"""CPU integration: keying. Reads the MXL grains of a v210a output and of a fill_key output back
(build/player_grain_dump) and checks alpha and key values: burn-ins (a moving box and a text box),
the transparent pattern, and an animated GIF and APNG played as a clip at their own frame timing.
`--shots DIR` also writes the fill and the key of each case as PNG."""

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
            "PLAYER_RAM_BUDGET_MB": "64",  # one budget for the process; a 20-frame clip below does not fit
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
        print("burn-ins ok", "v210a box/text/bg", box_a, text_a, bg_a, "key box/text/bg", kbox, ktext, kbg)

        # The transparent pattern: black at alpha 0 whatever the idle key; only the burn-ins are keyed.
        http("PATCH", "/api/v1/outputs/0", {"key_mode": "v210a", "idle_key": "opaque"})
        http("PUT", "/api/v1/outputs/0/source", {"type": "pattern", "pattern": "transparent", "audio": "silence"})
        time.sleep(0.6)
        st = http("GET", "/api/v1/outputs/0")
        g = grain(st["video_flow_id"], work)
        assert alpha(g, W // 2, H // 2) == 0 and luma(g, W // 2, H // 2) == 64, (alpha(g, W // 2, H // 2), luma(g, W // 2, H // 2))
        assert abs(alpha(g, 60, 20) - round(0.6 * 1023)) <= 1, alpha(g, 60, 20)
        shots(shot_dir, "transparent-burnin", g, lambda x, y: alpha(g, x, y) >> 2)

        # An animated GIF as a clip: two frames of 0.2 s, a red square left then a green one right on
        # a transparent background; at 25p each shows for 5 output frames, 10 per loop.
        gif = work / "anim.gif"
        subprocess.check_call(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
             "color=c=black@0.0:s=64x36:r=5:d=0.4,format=rgba,"
             "geq=r='if(N,0,255)':g='if(N,255,0)':b=0:a='if(between(X,8+N*32,23+N*32)*between(Y,8,23),255,0)'",
             "-vf", "split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse=alpha_threshold=128", "-loop", "0", str(gif)])
        # An APNG at 50 % alpha (GIF has 1-bit transparency only).
        apng = work / "half.png"
        subprocess.check_call(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
             "color=c=white:s=64x36:r=5:d=0.4,format=rgba,geq=r=255:g=255:b=255:a=128", "-plays", "0", "-f", "apng", str(apng)])

        def upload(path, clip=True):
            meta = http("POST", "/api/v1/uploads", {"name": path.name, "size": path.stat().st_size, "options": {"fit": "fit"}})
            http("PUT", f"/api/v1/uploads/{meta['id']}/chunks/0", raw=path.read_bytes())
            item = http("POST", f"/api/v1/uploads/{meta['id']}/complete", {})
            deadline = time.time() + 60
            while time.time() < deadline:
                conv = http("GET", f"/api/v1/library/{item['id']}")["conversions"].get("720p25", {})
                if conv.get("status") in ("ready", "failed"):
                    assert conv["status"] == "ready" and conv.get("clip") == clip, conv
                    return item, conv
                time.sleep(0.2)
            raise SystemExit("conversion timeout")

        gif_item, gif_conv = upload(gif)
        assert gif_item["type"] == "sprite" and gif_conv["frames"] == 2 and gif_conv["clip_frames"] == 10, gif_conv
        # Fired as any clip is: PUT the source, then the transport.
        http("PUT", "/api/v1/outputs/0/burnin", {"label": False, "timecode": False, "frame_counter": False, "item": False, "flow_id": False})
        http("PUT", "/api/v1/outputs/0/source", {"type": "video", "item_id": gif_item["id"]})
        http("POST", "/api/v1/outputs/0/transport", {"action": "restart"})
        deadline = time.time() + 10
        while not http("GET", "/api/v1/outputs/0")["media_ready"]:
            assert time.time() < deadline, "clip did not load"
            time.sleep(0.1)
        time.sleep(0.5)
        st = http("GET", "/api/v1/outputs/0")
        assert st["frames"] == 10 and st["loops"] >= 1 and st["media_error"] == "", st
        ram = st["ram_bytes"]
        assert ram == 2 * (STRIDE * H + ASTRIDE * H), ram

        def at(frame):
            # Pause first: a pause takes the playhead the writer showed last, a seek while paused holds its frame.
            http("POST", "/api/v1/outputs/0/transport", {"action": "pause"})
            http("POST", "/api/v1/outputs/0/seek", {"frame": frame})
            time.sleep(0.3)
            st = http("GET", "/api/v1/outputs/0")
            assert st["transport"] == "pause" and st["position"] == frame, (st["transport"], st["position"])
            return grain(st["video_flow_id"], work)

        # Fit: 64x36 to 1280x720 is 20x; square 1 at x 160..479, square 2 at x 800..1119, y 160..479.
        f0 = at(2)
        assert alpha(f0, 320, 320) == 1023 and alpha(f0, 960, 320) == 0 and alpha(f0, 640, 600) == 0
        assert rgb(f0, 320, 320)[0] > 200 and rgb(f0, 320, 320)[1] < 50, rgb(f0, 320, 320)
        shots(shot_dir, "gif-frame0", f0, lambda x, y: alpha(f0, x, y) >> 2)
        f1 = at(7)
        assert alpha(f1, 320, 320) == 0 and alpha(f1, 960, 320) == 1023
        assert rgb(f1, 960, 320)[1] > 200 and rgb(f1, 960, 320)[0] < 50, rgb(f1, 960, 320)
        f1b = at(4)
        assert alpha(f1b, 320, 320) == 1023  # frame 4 is still the first GIF frame
        http("POST", "/api/v1/outputs/0/transport", {"action": "play"})

        # The same clip in fill_key: the key at 940 under the square, 64 around.
        http("PATCH", "/api/v1/outputs/0", {"key_mode": "fill_key"})
        deadline = time.time() + 10
        while http("GET", "/api/v1/outputs/0")["ram_bytes"] != 2 * 2 * STRIDE * H:  # reloaded with the key
            assert time.time() < deadline, "clip did not load"
            time.sleep(0.1)
        f0 = at(2)
        st = http("GET", "/api/v1/outputs/0")
        key = grain(st["key_flow_id"], work)
        assert luma(key, 320, 320) == 940 and luma(key, 960, 320) == 64, (luma(key, 320, 320), luma(key, 960, 320))
        shots(shot_dir, "gif-fillkey", f0, lambda x, y: max(0, luma(key, x, y) - 64) * 255 // 876)
        http("POST", "/api/v1/outputs/0/transport", {"action": "play"})

        # An APNG with 50 % alpha: alpha 128/255 in the v210a plane.
        apng_item, apng_conv = upload(apng)
        assert apng_item["type"] == "sprite", apng_item["type"]
        http("PATCH", "/api/v1/outputs/0", {"key_mode": "v210a"})
        http("PUT", "/api/v1/outputs/0/source", {"type": "video", "item_id": apng_item["id"]})
        deadline = time.time() + 10
        while not http("GET", "/api/v1/outputs/0")["media_ready"]:
            assert time.time() < deadline, "clip did not load"
            time.sleep(0.1)
        time.sleep(0.4)
        g = grain(http("GET", "/api/v1/outputs/0")["video_flow_id"], work)
        apng_a = alpha(g, 640, 360)
        assert abs(apng_a - round(128 / 255 * 1023)) <= 5, apng_a  # one 8-bit step of scaling
        assert http("GET", "/api/v1/outputs/0")["underruns"] == 0

        # A GIF whose clip was refused (over the budget), with the idle key opaque: the output is
        # transparent, not an opaque black frame, and says why. Stop shows the idle key.
        big = work / "big.gif"
        subprocess.check_call(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=blue:s=640x360:r=10",
                               "-frames:v", "20", str(big)])
        big_item, big_conv = upload(big, clip=False)
        assert "PLAYER_RAM_BUDGET_MB" in big_conv["clip_error"], big_conv
        http("PATCH", "/api/v1/outputs/0", {"idle_key": "opaque"})
        http("PUT", "/api/v1/outputs/0/source", {"type": "video", "item_id": big_item["id"]})
        http("POST", "/api/v1/outputs/0/transport", {"action": "play"})
        deadline = time.time() + 10
        while not http("GET", "/api/v1/outputs/0")["media_error"]:
            assert time.time() < deadline, "no media_error"
            time.sleep(0.1)
        time.sleep(0.3)
        st = http("GET", "/api/v1/outputs/0")
        g = grain(st["video_flow_id"], work)
        assert not st["media_ready"] and alpha(g, 640, 360) == 0, (st["media_error"], alpha(g, 640, 360))
        http("POST", "/api/v1/outputs/0/transport", {"action": "stop"})
        time.sleep(0.3)
        g = grain(st["video_flow_id"], work)
        assert alpha(g, 640, 360) == 1023, alpha(g, 640, 360)
        print("keying ok", "refused clip:", st["media_error"])
        print("keying ok", "gif frames", gif_conv["frames"], "loop", gif_conv["clip_frames"], "ram", ram, "apng alpha", apng_a)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()


if __name__ == "__main__":
    sys.exit(main())
