# mxl-test-player

TAI-locked MXL test source. Each output writes a `video/v210` flow and an `audio/float32` flow (optional `video/smpte291` timecode and a fill/key pair) and publishes them as an NMOS sender group.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd web && npm install && npm run build && cd ..
./build/mxl-test-player
```

Open `http://localhost:8130`.

Container images are `ghcr.io/leeo86/mxl-test-player`. Pushes to `main` publish `nightly-dev` and `git-<sha7>`. A tag `vX.Y.Z` publishes `X.Y.Z`, `X.Y` and `X`. `nightly-dev` is the only tag that moves. The workflow is `.github/workflows/container.yaml` and the image is `docker/Dockerfile` (uid 1000).

```bash
docker build -f docker/Dockerfile -t mxl-test-player .
```

## Run on the platform

The platform mounts `/Volumes/mxl`, sets uid 1000, and injects the pod IP as `NMOS_HOST_ADDRESS`. Use the pod network. Set `NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS`, the registry and the Query API (registration port + 1), `MXL_OUTPUT_DOMAIN_DIR`, `MXL_CLEANUP_ON_EXIT=true` and a writable `/config`. `deploy/k8s/deployment.yaml` is an example. `production-up` waits for `/readyz`, which stays 503 until the Query API lists this node.

## Settings

Precedence is environment, then the JSON file (`--config`, else `PLAYER_CONFIG`, else `$CONFIG_DIR/player.json`), then the defaults. Unknown variables are ignored. Invalid values exit 78. A port that cannot be bound exits 75. SIGTERM exits 143. A clean stop exits 0.

| Setting | Default | Meaning |
| --- | --- | --- |
| `CONFIG_DIR` | `/config` | Only directory the process writes for its own state |
| `PLAYER_CONFIG` | `$CONFIG_DIR/player.json` | Config file path. Alias of `--config` |
| `PLAYER_STATE` | `$CONFIG_DIR/state.json` | Alias of the state file path |
| `PLAYER_FORMAT` | `1080p50` | Platform format (`1080i25` and `1080i29.97` are top field first; `1080i25bff` and `1080i29.97bff` bottom field first) |
| `PLAYER_OUTPUTS` | `2` | Output count, 1..16 |
| `PLAYER_AUDIO_CHANNELS` | `16` | Default audio channels |
| `PLAYER_LIBRARY_DIR` | `/data/library` | Mezzanine library; one player per directory (Kubernetes: `strategy: Recreate`). Items missing from its `index.json` are restored from their `item.json` at start |
| `PLAYER_IMPORT_DIR` | empty | Watched import directory |
| `PLAYER_CONVERT_CONCURRENCY` | `1` | ffmpeg jobs at once |
| `PLAYER_RAM_CLIP_MAX_S` | `20` | Longest clip kept in RAM |
| `PLAYER_RAM_BUDGET_MB` | `4096` | One RAM budget for the whole process: every output's clips in RAM (videos up to `PLAYER_RAM_CLIP_MAX_S`, GIF and APNG clips; shared between outputs that play the same clip). Also bounds an animation's conversion |
| `PLAYER_PREROLL_FRAMES` | `25` | Frames prepared before play |
| `PLAYER_FONT_DIR` | bundled | Font directory |
| `PLAYER_WEB_ROOT` | bundled | Built web UI |
| `PLAYER_UPLOAD_LIMIT_GB` | `20` | Upload limit |
| `PLAYER_SPRITE_MAX_PX` | `512` | Moving-box sprite size (an animation's sprite frames) |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | Parent of domain directories. No inputs are scanned |
| `MXL_OUTPUT_DOMAIN_DIR` | `<scan>/player-<seed>` | This function's domain. Created if missing |
| `MXL_OUTPUT_DOMAIN_ID` | UUIDv5(seed, `domain`) | Used when the domain is created; a different existing id is logged and kept |
| `MXL_HISTORY_DURATION_NS` | `1000000000` | Written into `options.json` only when that file is created |
| `MXL_CLEANUP_ON_EXIT` | `false` | Remove only this domain on shutdown |
| `NMOS_REGISTRY_ADDRESS` | empty | Registration API host. Empty disables registration |
| `NMOS_REGISTRY_PORT` | `3210` | Registration API port |
| `NMOS_QUERY_ADDRESS` | registry address | Query API host |
| `NMOS_QUERY_PORT` | registry port + 1 | Query API port |
| `NMOS_DNS_SD` | `false` | `true` exits 78. No Avahi and no mDNS |
| `NMOS_PORT` | `3282` | IS-04 node API and IS-05 connection API |
| `NMOS_SEED` | `$HOST_ID-player`; without `HOST_ID` a random `player-…` kept in `CONFIG_DIR/nmos_seed` (the hostname when that is not writable), with a warning | UUIDv5 root for every id. Set it: two players with the same seed share node and device ids and remove each other's resources. Sender, source and flow ids also depend on the output's position (Out 1, Out 2, …), so reordering outputs changes them |
| `NMOS_LABEL` | `MXL Test Player` | Node label and device label |
| `NMOS_TAGS` | `{}` | JSON object of tag name to string array |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | Address announced to NMOS. Not a name, `0.0.0.0` or loopback |
| `HOST_ID` | unset | Legacy seed prefix when `NMOS_SEED` is unset. Not an address |
| `SHUTDOWN_TIMEOUT_S` | `10` | Bound for SIGTERM cleanup |
| `WEB_PORT` | `8130` | UI, REST, probes, metrics, `/api/v1/events`, and the widgets |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | CSP `frame-ancestors` of the `/widget` pages; `/widgets` answers these origins with CORS. `;`, `,` or control characters exit 78 |

`NMOS_HOST_ADDRESS` is the node `href`, `api.endpoints[].host` and the IS-05 control href. `HOST_ID` is not announced. The UI WebSocket is on `WEB_PORT`. This process does not listen on `NMOS_PORT+1`.

## Exit codes

| Code | When |
| --- | --- |
| 0 | Clean stop (SIGINT) |
| 75 | The web or NMOS port could not be bound, or the MXL domain could not be opened |
| 78 | Invalid configuration, including a bad address, `NMOS_DNS_SD=true`, or a `domain_def.json` without an id |
| 143 | SIGTERM after cleanup, or cleanup that exceeded `SHUTDOWN_TIMEOUT_S` |

## API

| Method | Path |
| --- | --- |
| GET | `/livez`, `/readyz`, `/statusz`, `/metrics` |
| GET | `/api/v1/info`, `/api/v1/config`, `/api/v1/config/export`, `/api/v1/nmos` |
| POST | `/api/v1/config/import` |
| GET, PATCH | `/api/v1/outputs`, `/api/v1/outputs/{id}` (also `master_video`, `master_audio`, `master_data`, `master_key`) |
| POST | `/api/v1/outputs/{id}/transport` (`play`, `pause`, `stop`, `restart`, `step`), `/api/v1/outputs/{id}/seek` |
| PUT | `/api/v1/outputs/{id}/source`, `/api/v1/outputs/{id}/burnin` |
| GET | `/api/v1/outputs/{id}/thumbnail`, `/api/v1/outputs/{id}/probe[?index=]`, `/api/v1/library/{id}/thumbnail` |
| GET, POST, DELETE | `/api/v1/library`, `/api/v1/presets`, `/api/v1/playlists` |
| PATCH, POST | `/api/v1/library/{id}` (name, tags), `/api/v1/library/{id}/reconvert` |
| PUT | `/api/v1/playlists/{id}` |
| POST, PUT | `/api/v1/uploads`, `/api/v1/uploads/{id}/chunks/{n}`, `/api/v1/uploads/{id}/complete` |
| GET | `/api/v1/jobs` |
| WebSocket | `/api/v1/events` on `WEB_PORT` |
| GET | `/widgets`, `/widget/transport?output=<n>[&theme=dark\|light\|transparent]` (below) |
| GET, PATCH | `/x-nmos/node/v1.3/…`, `/x-nmos/connection/v1.1/single/senders/{id}/staged` on `NMOS_PORT` |

Fire a clip with `PUT /api/v1/outputs/{id}/source` (`{"type": "video", "item_id": "<id>"}`, or `still`, `pattern`, `playlist`) and `POST /api/v1/outputs/{id}/transport` (`{"action": "restart"}`). An animated GIF or APNG (`sprite` item) plays the same way, as a loop with its alpha (`v210a` or `fill_key`); in a playlist set through the API its entry needs `frames` = the item's `clip_frames` for the output's format. It is converted per format into raw frames: about 8 MB of library disk per GIF frame and format at 1080 lines (33 MB at 2160). The `transparent` pattern is black at alpha 0, for a keyed output of burn-ins only. An output that feeds a keyer with graphics should set `idle_key: transparent` (patterns, videos and stop carry the idle key; an item with alpha that is not ready is transparent).

`GET /api/v1/config/export` is one JSON document (`version`, informational `deployment`, and `state`). There are no secrets to omit. `POST /api/v1/config/import` restores outputs, presets and playlists. It does not change ports, the registry or the seed. `GET /api/v1/config` remains the live view.

`GET /api/v1/config` also lists every setting with its effective value and origin (`environment`, `file`, `argument` for `--config`, `default`). `GET /api/v1/info` has the version and the MXL revision.

The web UI (tabs Outputs, Source, Burn-ins, Library, Patterns, Playlists, NMOS, Status, Settings) uses only this API.

Widgets: operator screens can frame one output's transport. `GET /widgets` lists them as `[{id, title, params, min_size, version}]` (`params` is a JSON schema of the query). `GET /widget/transport?output=<n>` (1 is Out 1; `min_size` 420×160) shows the output's label, format, state and what it plays, with Play/Pause/Stop, Restart, Step, Loop and the position (click to seek a video). The page uses the player's own API and posts `widget-ready` and `widget-size` to its parent. Only the pages that `WIDGET_FRAME_ANCESTORS` names may frame it; on the platform set it to `'self' https://designer.<fqdn>`. `cd web && PLAYER_API=http://<player>:8130 npm run dev` runs it against a running player.

`/readyz` is 200 when the process is serving and, if a registry is configured, `GET /x-nmos/query/v1.3/nodes/{id}` on the Query API returns 200.

## Tests

```bash
./build/player_tests
python3 tests/integration/lifecycle.py
python3 tests/integration/play_loop.py
python3 tests/integration/keying.py [--shots DIR]
```

`keying.py` reads the MXL grains back with `build/player_grain_dump <domain> <flow id> <index|-N> <file>` (a lab tool built with the tests) and checks the alpha and key values.

See `IMPLEMENTATION_PLAN.md` for the G1–G14 audit and `deploy/` for Compose, Kubernetes and Grafana.
