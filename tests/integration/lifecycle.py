#!/usr/bin/env python3
"""Start, wait until the node is registered, SIGTERM, and check cleanup."""

import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / "build" / "mxl-test-player"
WEB = 18230
NMOS = 18282
REG = 18310
QUERY = 18311


class Registry(BaseHTTPRequestHandler):
    nodes = set()
    deleted = []
    query_ok = False
    lock = threading.Lock()

    def log_message(self, fmt, *args):
        return

    def _code(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        if self.path.startswith("/x-nmos/registration/v1.3/resource"):
            try:
                doc = json.loads(raw.decode() or "{}")
                data = doc.get("data") or {}
                if doc.get("type") == "node" and data.get("id"):
                    with self.lock:
                        self.nodes.add(data["id"])
                    href = data.get("href", "")
                    if "127.0.0.1" in href or "://" not in href:
                        self._code(400, b"bad href")
                        return
            except json.JSONDecodeError:
                self._code(400, b"bad json")
                return
            self._code(201, b"{}")
            return
        if self.path.startswith("/x-nmos/registration/v1.3/health/"):
            self._code(200, b"")
            return
        self._code(404, b"")

    def do_DELETE(self):
        with self.lock:
            self.deleted.append(self.path)
            node = self.path.rsplit("/", 1)[-1]
            self.nodes.discard(node)
        self._code(204, b"")

    def do_GET(self):
        if self.path.startswith("/x-nmos/query/v1.3/nodes/"):
            node = self.path.rstrip("/").rsplit("/", 1)[-1]
            with self.lock:
                ok = self.query_ok and node in self.nodes
            if ok:
                self._code(200, json.dumps({"id": node}).encode())
            else:
                self._code(404, b"")
            return
        self._code(404, b"")


def http(method, path, body=None, timeout=5):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{WEB}{path}", data=data, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            return res.status, res.read()
    except urllib.error.HTTPError as ex:
        return ex.code, ex.read()


def main():
    work = Path(f"/dev/shm/mtp-life-{os.getpid()}")
    lib = Path(f"/tmp/mtp-life-lib-{os.getpid()}")
    cfg = Path(f"/tmp/mtp-life-cfg-{os.getpid()}")
    if work.exists():
        subprocess.check_call(["rm", "-rf", str(work)])
    lib.mkdir(parents=True, exist_ok=True)
    (lib / "_uploads" / "stale").mkdir(parents=True)
    cfg.mkdir(parents=True, exist_ok=True)

    server = ThreadingHTTPServer(("127.0.0.1", REG), Registry)
    query = ThreadingHTTPServer(("127.0.0.1", QUERY), Registry)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    threading.Thread(target=query.serve_forever, daemon=True).start()

    env = os.environ.copy()
    env.update(
        {
            "PLAYER_FORMAT": "720p25",
            "PLAYER_OUTPUTS": "1",
            "PLAYER_AUDIO_CHANNELS": "2",
            "PLAYER_LIBRARY_DIR": str(lib),
            "CONFIG_DIR": str(cfg),
            "MXL_DOMAIN_SCAN_PATH": "/dev/shm",
            "MXL_OUTPUT_DOMAIN_DIR": str(work),
            "MXL_CLEANUP_ON_EXIT": "true",
            "SHUTDOWN_TIMEOUT_S": "8",
            "NMOS_SEED": "life-player",
            "NMOS_LABEL": "Life",
            "NMOS_TAGS": '{"urn:x-srf:production":["life"],"urn:x-srf:function":["player"]}',
            "NMOS_HOST_ADDRESS": "10.9.8.7",
            "NMOS_REGISTRY_ADDRESS": "127.0.0.1",
            "NMOS_REGISTRY_PORT": str(REG),
            "NMOS_QUERY_ADDRESS": "127.0.0.1",
            "NMOS_QUERY_PORT": str(QUERY),
            "NMOS_DNS_SD": "false",
            "NMOS_PORT": str(NMOS),
            "WEB_PORT": str(WEB),
        }
    )
    proc = subprocess.Popen([str(BIN)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        saw_503 = False
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                code, _ = http("GET", "/readyz", timeout=1)
            except Exception:
                time.sleep(0.1)
                continue
            if code == 503:
                saw_503 = True
                break
            if code == 200:
                break
            time.sleep(0.1)
        if not saw_503:
            raise SystemExit("expected /readyz 503 before the Query API lists the node")
        Registry.query_ok = True
        deadline = time.time() + 15
        while time.time() < deadline:
            code, _ = http("GET", "/readyz", timeout=2)
            if code == 200:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("readyz did not become 200 after registration")

        code, body = http("GET", "/api/v1/config/export")
        if code != 200:
            raise SystemExit(f"export failed {code}")
        doc = json.loads(body)
        if doc.get("version") != 1 or "state" not in doc:
            raise SystemExit(f"unexpected export {doc}")
        code, _ = http("POST", "/api/v1/config/import", doc)
        if code != 200:
            raise SystemExit(f"import failed {code}")

        nmos = urllib.request.urlopen(f"http://127.0.0.1:{NMOS}/x-nmos/node/v1.3/self", timeout=2)
        self_doc = json.loads(nmos.read())
        if "10.9.8.7" not in self_doc.get("href", "") or self_doc.get("hostname") != "10.9.8.7":
            raise SystemExit(f"announced a name instead of the address: {self_doc}")
        if "urn:x-srf:production" not in self_doc.get("tags", {}):
            raise SystemExit(f"tags missing: {self_doc.get('tags')}")

        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
            raise SystemExit("SIGTERM did not exit within SHUTDOWN_TIMEOUT_S")
        if rc != 143:
            raise SystemExit(f"exit {rc}, want 143")
        if work.exists():
            raise SystemExit(f"own domain was not removed: {work}")
        if (lib / "_uploads").exists():
            raise SystemExit("_uploads was not removed")
        if not Registry.deleted:
            raise SystemExit("registry did not see a node DELETE")
        print("lifecycle ok", Registry.deleted[-1])
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGKILL)
            proc.wait(timeout=5)
        server.shutdown()
        query.shutdown()


if __name__ == "__main__":
    try:
        main()
    except Exception as ex:
        print(ex, file=sys.stderr)
        sys.exit(1)
