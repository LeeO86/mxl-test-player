#!/usr/bin/env python3
"""An output change leaves exactly one sender per role in the registry.

With the stand-in registry (registry_stub.py): switch Out 2 from v210 to v210a, then
its format, then ANC and key off and on. Each time the sender keeps its id, points at
the new flow, the old flow is DELETEd from the registry and from the MXL domain, and
no sender or flow refers to a resource that is gone. Then a run that ends without
the node DELETE (SIGKILL) and a restart with another format: the new run removes the
flows the old one left.
"""

import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import registry_stub  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "mxl-test-player"
WEB = 18430
NMOS = 18482
REG = 18510


def http(method, path, body=None, timeout=5):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{WEB}{path}", data=data, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            raw = res.read()
            try:
                return res.status, json.loads(raw or b"null")
            except json.JSONDecodeError:
                return res.status, raw
    except urllib.error.HTTPError as ex:
        return ex.code, ex.read()


def wait_for(what, check, timeout=4.0):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = check()
        if last is True:
            return time.time()
        time.sleep(0.05)
    raise SystemExit(f"timed out: {what}: {last}")


def consistent(store, outputs):
    """True, or what is wrong: one sender per output and role, each on a live flow and source."""
    snap = store.snapshot()
    senders, flows, sources = snap["senders"], snap["flows"], snap["sources"]
    want = set()
    for o in outputs:
        roles = ["video", "audio"] + (["data"] if o["anc"] else []) + (["key"] if o["key_mode"] == "fill_key" else [])
        for r in roles:
            want.add((o[f"{r}_sender_id"], o[f"{r}_flow_id"]))
    have = {(s["id"], s["flow_id"]) for s in senders.values()}
    if have != want:
        return f"senders {sorted(have)} != {sorted(want)}"
    if set(flows) != {f for _, f in want}:
        return f"flows {sorted(flows)} != {sorted(f for _, f in want)}"
    for f in flows.values():
        if f["source_id"] not in sources:
            return f"flow {f['id']} on missing source {f['source_id']}"
    if len(sources) != len(want):
        return f"{len(sources)} sources for {len(want)} senders"
    labels = [s["label"] for s in senders.values()]
    if len(labels) != len(set(labels)):
        return f"duplicate sender labels {sorted(labels)}"
    return True


def start(env, log):
    return subprocess.Popen([str(BIN)], env=env, stdout=log, stderr=subprocess.STDOUT)


def main():
    pid = os.getpid()
    work = Path(f"/dev/shm/mtp-stale-{pid}")
    lib = Path(f"/tmp/mtp-stale-lib-{pid}")
    cfg = Path(f"/tmp/mtp-stale-cfg-{pid}")
    for d in (lib, cfg):
        d.mkdir(parents=True, exist_ok=True)
    store, _server = registry_stub.serve(REG)

    env = os.environ.copy()
    env.update(
        {
            "PLAYER_FORMAT": "720p25",
            "PLAYER_OUTPUTS": "2",
            "PLAYER_AUDIO_CHANNELS": "2",
            "PLAYER_LIBRARY_DIR": str(lib),
            "CONFIG_DIR": str(cfg),
            "PLAYER_FONT_DIR": str(ROOT / "assets" / "fonts"),
            "MXL_DOMAIN_SCAN_PATH": "/dev/shm",
            "MXL_OUTPUT_DOMAIN_DIR": str(work),
            "MXL_CLEANUP_ON_EXIT": "true",
            "SHUTDOWN_TIMEOUT_S": "8",
            "NMOS_SEED": "stale-player",
            "NMOS_HOST_ADDRESS": "10.9.8.7",
            "NMOS_REGISTRY_ADDRESS": "127.0.0.1",
            "NMOS_REGISTRY_PORT": str(REG),
            "NMOS_QUERY_ADDRESS": "127.0.0.1",
            "NMOS_QUERY_PORT": str(REG),
            "NMOS_PORT": str(NMOS),
            "WEB_PORT": str(WEB),
        }
    )
    log = open(cfg / "player.log", "w")
    proc = start(env, log)
    try:
        def ready():
            try:
                return http("GET", "/readyz", timeout=1)[0] == 200 or "not ready"
            except Exception as ex:  # not listening yet
                return str(ex)

        wait_for("ready", ready, 20)

        def outputs():
            return http("GET", "/api/v1/outputs")[1]

        wait_for("first registration", lambda: consistent(store, outputs()), 8)
        first = outputs()
        before = first[1]
        assert before["label"] == "Out 2" and before["key_mode"] == "off", before

        def step(name, patch):
            prev = outputs()[1]
            del store.deleted[:]
            code, out = http("PATCH", "/api/v1/outputs/1", patch)
            assert code == 200, (code, out)
            t0 = time.time()
            wait_for(name, lambda: consistent(store, outputs()))
            # Not at the next 5 s heartbeat: the change wakes the registration loop.
            assert time.time() - t0 < 3, f"{name}: registry updated after {time.time() - t0:.1f} s"
            cur = outputs()[1]
            for r in ("video", "audio", "data", "key"):
                assert cur[f"{r}_sender_id"] == prev[f"{r}_sender_id"], f"{name}: {r} sender id changed"
            print(f"{name}: deleted {sorted(store.deleted)}")
            return prev, cur

        # The case seen on small: Out 2 from v210 to v210a. Same sender, new flow, old flow deleted.
        prev, cur = step("v210 -> v210a", {"key_mode": "v210a"})
        assert cur["video_flow_id"] != prev["video_flow_id"]
        assert f"flows/{prev['video_flow_id']}" in store.deleted, store.deleted
        assert not any(d.startswith("senders/") for d in store.deleted), store.deleted
        old_dir = work / f"{prev['video_flow_id']}.mxl-flow"
        new_dir = work / f"{cur['video_flow_id']}.mxl-flow"
        wait_for("old MXL flow removed", lambda: (not old_dir.exists() and new_dir.exists()) or f"old {old_dir.exists()} new {new_dir.exists()}")
        snap = store.snapshot()
        assert snap["senders"][cur["video_sender_id"]]["flow_id"] == cur["video_flow_id"]
        assert sum(1 for s in snap["senders"].values() if s["label"] == "Out 2 Video") == 1

        prev, cur = step("format 1080p25", {"format": "1080p25"})
        assert {f"flows/{prev[k]}" for k in ("video_flow_id", "audio_flow_id", "data_flow_id")} <= set(store.deleted), store.deleted

        prev, cur = step("ANC off", {"anc": False})
        # The sender first, then its flow, then its source (the status lists no source ids).
        assert len(store.deleted) == 3, store.deleted
        assert store.deleted[:2] == [f"senders/{prev['data_sender_id']}", f"flows/{prev['data_flow_id']}"], store.deleted
        assert store.deleted[2].startswith("sources/"), store.deleted
        step("fill_key", {"key_mode": "fill_key", "anc": True})
        prev, cur = step("key off, back to v210", {"key_mode": "off"})
        assert store.deleted[0] == f"senders/{prev['key_sender_id']}", store.deleted

        # A run that ends without the node DELETE, then a start with another format.
        left = set(store.snapshot()["flows"])
        del store.deleted[:]
        proc.send_signal(signal.SIGKILL)
        proc.wait(timeout=5)
        assert set(store.snapshot()["flows"]) == left
        env["PLAYER_FORMAT"] = "1080p50"
        proc = start(env, log)
        wait_for("ready after restart", ready, 20)
        wait_for("restart registration", lambda: consistent(store, outputs()), 8)
        now = outputs()
        assert now[0]["format"] == "1080p50" and now[0]["video_sender_id"] == first[0]["video_sender_id"], now[0]
        assert f"flows/{first[0]['video_flow_id']}" in store.deleted, store.deleted
        print("stale senders ok:", len(store.snapshot()["senders"]), "senders,", len(store.snapshot()["flows"]), "flows")
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    if store.snapshot()["nodes"]:
        raise SystemExit("node not deleted at SIGTERM")
    if work.exists():
        raise SystemExit(f"own domain {work} left behind")
    subprocess.call(["rm", "-r", "--", str(lib), str(cfg)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
