#!/usr/bin/env python3
"""A stand-in IS-04 v1.3 registry: Registration API and Query API on one port.

It keeps every resource a node POSTs (node, device, source, flow, sender), answers
DELETE of one resource, and a node DELETE removes the node's children, as a registry
does. The Query API lists each type, filtered by `?key=value` basic queries, and
GET /x-nmos/query/v1.3/<type>/<id> returns one resource.

Run on its own (lab checks): registry_stub.py [port]  (default 18510, 127.0.0.1)
"""

import json
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TYPES = ("nodes", "devices", "sources", "flows", "senders", "receivers")


class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.res = {t: {} for t in TYPES}
        self.deleted = []  # "<type>/<id>" in the order the DELETEs came
        self.fail_lists = 0  # answer this many Query API list GETs with 503 (a Query API that is down)

    def snapshot(self):
        with self.lock:
            return {t: dict(v) for t, v in self.res.items()}

    def delete_node(self, node_id):
        devices = [d for d, r in self.res["devices"].items() if r.get("node_id") == node_id]
        for t in ("sources", "flows", "senders", "receivers"):
            for i in [i for i, r in self.res[t].items() if r.get("device_id") in devices]:
                del self.res[t][i]
        for d in devices:
            del self.res["devices"][d]
        self.res["nodes"].pop(node_id, None)


def make_handler(store):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            return

        def _send(self, code, body=None):
            raw = b"" if body is None else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Connection", "close")
            self.end_headers()
            if raw:
                self.wfile.write(raw)
            self.close_connection = True

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0") or 0)
            raw = self.rfile.read(length) if length else b""
            path = self.path.split("?", 1)[0]
            if path == "/x-nmos/registration/v1.3/resource":
                try:
                    doc = json.loads(raw.decode() or "{}")
                except json.JSONDecodeError:
                    return self._send(400, {"error": "bad json"})
                t = (doc.get("type") or "") + "s"
                data = doc.get("data") or {}
                if t not in TYPES or not data.get("id"):
                    return self._send(400, {"error": "bad resource"})
                with store.lock:
                    existed = data["id"] in store.res[t]
                    store.res[t][data["id"]] = data
                return self._send(200 if existed else 201, data)
            if path.startswith("/x-nmos/registration/v1.3/health/nodes/"):
                node = path.rsplit("/", 1)[-1]
                with store.lock:
                    known = node in store.res["nodes"]
                return self._send(200 if known else 404, {"health": "0"})
            return self._send(404, {"error": "not found"})

        def do_DELETE(self):
            parts = self.path.split("?", 1)[0].strip("/").split("/")
            # x-nmos/registration/v1.3/resource/<type>/<id>
            if len(parts) == 6 and parts[:4] == ["x-nmos", "registration", "v1.3", "resource"] and parts[4] in TYPES:
                t, i = parts[4], parts[5]
                with store.lock:
                    store.deleted.append(f"{t}/{i}")
                    if i not in store.res[t]:
                        return self._send(404, {"error": "not found"})
                    if t == "nodes":
                        store.delete_node(i)
                    else:
                        del store.res[t][i]
                return self._send(204)
            return self._send(404, {"error": "not found"})

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            parts = url.path.strip("/").split("/")
            if len(parts) >= 4 and parts[:3] == ["x-nmos", "query", "v1.3"] and parts[3] in TYPES:
                t = parts[3]
                with store.lock:
                    items = list(store.res[t].values())
                if len(parts) == 4:
                    with store.lock:
                        failing = store.fail_lists > 0
                        if failing:
                            store.fail_lists -= 1
                    if failing:
                        return self._send(503, {"error": "unavailable"})
                if len(parts) == 5:
                    hit = [r for r in items if r.get("id") == parts[4]]
                    return self._send(200, hit[0]) if hit else self._send(404, {"error": "not found"})
                for k, v in urllib.parse.parse_qsl(url.query):
                    if not k.startswith("paging."):
                        items = [r for r in items if str(r.get(k)) == v]
                return self._send(200, items)
            return self._send(404, {"error": "not found"})

    return Handler


def serve(port, host="127.0.0.1"):
    store = Store()
    server = ThreadingHTTPServer((host, port), make_handler(store))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return store, server


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18510
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    serve(port, host)
    print(f"registry stub on {host}:{port}", flush=True)
    threading.Event().wait()
