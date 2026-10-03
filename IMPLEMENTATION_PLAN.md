# Implementation plan

Status: v1 implemented against draft spec v0.3. Deviations are recorded here, as the spec requires.

## Decisions

- **Image decode** uses FFmpeg/libav (PNG, JPEG, TIFF, WebP, GIF, APNG). No separate image library.
- **Mezzanine** is ProRes 422 HQ (`prores_ks` profile 3) in MOV with PCM float32 audio. Measured on this host: a 0.4 s 640×360 source converts to 720p25 ProRes and plays from RAM with zero underruns (see `tests/integration/play_loop.py`).
- **Text** is FreeType with bundled DejaVu Sans, DejaVu Sans Bold, and DejaVu Sans Mono.
- **Interlaced MXL grains are fields.** Pinned MXL `218ddaa` (`FlowParser`) requires `grain_rate` 25/1 or 30000/1001 in the flow JSON for `interlaced_tff`/`interlaced_bff`, then doubles it so each grain is one field. The spec's parenthetical "grain = frame" does not match that SDK. Picture burn-in timecode stays at the frame rate; the ANC field flag marks the field. 1080i50 in the integration wording is treated as 1080i25 (50 fields).
- **59.94 audio cadence is 800/801 samples**, not 1601/1602. 1601/1602 is the 29.97 frame cadence. Both are tested. The count is the truncating sum `samples(n+1) - samples(n)` with `samples(n) = n * 48000 * den / num`, which is exact at the 1001-frame cycle.
- **v210a** is implemented as the pinned SDK lays it out: v210 fill plane followed by a 10-bit alpha plane (`(width+2)/3*4` bytes per line). `mxl-multiviewer` and Strom were not in this environment, so receiver support is unverified. `fill_key` (two `video/v210` flows) is the default keyed mode. The v210 key uses legal luma 64–940; the v210a alpha plane uses full-range 0–1023.
- **NMOS** is an in-process IS-04 v1.3 node plus IS-05 v1.1 single-sender connection API with BCP-007-03 MXL transport (`urn:x-nmos:transport:mxl`, empty `interface_bindings`, null manifest, `mxl_domain_id` / `mxl_flow_id`). `nmos-cpp` is not linked. DNS-SD is off. `NMOS_DNS_SD=true` exits 78 because this build has no browse and no mDNS advertisement. Registration is HTTP to `NMOS_REGISTRY_ADDRESS`. `/readyz` waits until the Query API lists the node. SIGTERM DELETEs the node.
- **FFmpeg** on the image used to develop this tree is 6.1 (libavcodec 60). The code uses the 6.x decode API (`AVChannelLayout`, `AVFrame::duration`). A 7.x runtime is the spec pin; the Dockerfile builds against the distro FFmpeg and records that gap.
- **Conversion** runs `ffmpeg` for the video mezzanine (bwdif for interlaced sources, `fps` or `minterpolate`, `tinterlace` when the target is interlaced, `colorspace` when the source is tagged BT.601) and libav for stills and sprites. Audio is resampled, optionally `loudnorm`'d to −23 LUFS, then conformed in-process so the sample count equals the frame cadence. Channel mapping keeps source channels and pads with silence.
- **RAM clips** are used when duration ≤ `PLAYER_RAM_CLIP_MAX_S` and the v210 payload fits `PLAYER_RAM_BUDGET_MB`. Longer files are not decode-ahead queued in this revision: playback of those items repeats black and counts underruns until a RAM-sized conversion is available. Patterns, stills, and RAM clips are written on the TAI clock with a repeat-last underrun path.
- **Moving-box continuity.** Position is a pure function of the TAI grain index. A parameter edit stores a pixel delta so the box does not jump; that delta is part of output state, so a restart reproduces it. With a zero delta the position matches across restarts.
- **Performance targets** (4×1080p50 for 1 h, 2×2160p50, 16× RAM) were not run on a Precision 3930 or R740. A single 720p25 pattern output on this container wrote grains with zero underruns over the integration window.
- **Lab run 2026-10-03** (2× Xeon Gold 6136, this tree with the fixes in the CHANGELOG `Unreleased` section; 1.0.0 reached about 19 grains/s per 1080p50 pattern output):

  | Case | Grains/s per output | Underruns | Process CPU |
  | --- | --- | --- | --- |
  | 4 × 1080p50 pattern, 120 s, quiet host | 50.0 | 11 | 2.0 cores |
  | 4 × 1080p50 pattern, 1 h, host busy with builds and other benchmarks | 49.4 | about 2300 per output | – |
  | 2 × 2160p50 pattern, 120 s | 34.9 | 3640 | 2.0 cores |
  | 16 × 1080p50 pattern, 120 s | 49.9 | 286 | 9.1 cores |
  | 16 × 1080p50 RAM clip (10 s ProRes mezzanine, same item), 120 s | 49.8 | 520 | 8.4 cores |

  2160p50 misses: one core per output, about 29 ms per grain, mostly the burn-in (`composite_rgba_onto_v210`, `unpack_v210_line`, per-pixel `rgb_to_yuv709`, `pack_v210_line`). Each output keeps its own RAM copy of a clip, so 16 outputs of one 10 s item hold 44 GB. The ProRes long-item case is still not implemented (see above).

## Build

MXL `release/v1.1` commit `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` is fetched at configure time and compiled with fabrics off. fmt, spdlog, stduuid, and picojson are fetched the same way. The player links that static `libmxl`.

Container publish matches the sibling media functions. `.github/workflows/container.yaml` builds `docker/Dockerfile` and pushes `ghcr.io/leeo86/mxl-test-player` (`nightly-dev` and `git-<sha7>` from `main`; `X.Y.Z`, `X.Y` and `X` from a `vX.Y.Z` tag). `nightly-dev` is the only moving tag. Pull requests build the image and do not push it. Labels include `org.opencontainers.image.source`, `.revision`, `.licenses` and `io.dmf.mxl.revision` (the pinned MXL commit). The runtime image runs as uid 1000 and carries the bundled web UI, DejaVu fonts, and the distro FFmpeg CLI.

## Platform guideline G1–G14

| Requirement | Status | Evidence | Change |
| --- | --- | --- | --- |
| G1 Configuration | met | `src/config.cpp:330`, `src/main.cpp:31` | Env, then JSON, then defaults. Invalid values throw `ConfigError` and exit 78. State is under `CONFIG_DIR` (default `/config`). `PLAYER_STATE` and `PLAYER_CONFIG` remain aliases. No secrets are stored. |
| G2 MXL domains | met | `src/mxl_io.cpp:42`, `src/config.cpp:370` | `MXL_DOMAIN_SCAN_PATH` defaults to `/Volumes/mxl` and is the parent of the default output domain. This player has no input flows, so it does not scan or open other domains. `domain_def.json` and `options.json` are created once; a mismatched id exits 78 and is not overwritten. `MXL_HISTORY_DURATION_NS` is written only when `options.json` is created. |
| G3 NMOS identity | met | `src/ids.cpp`, `src/nmos.cpp:132` | `NMOS_SEED` is the UUIDv5 root for the node, device, sources, flows, senders and the default domain id. `NMOS_LABEL` and `NMOS_TAGS` are on the node and device. Group hints stay on the flows. |
| G4 Registry, no DNS-SD | met | `src/config.cpp:383`, `src/nmos.cpp` | `NMOS_QUERY_ADDRESS` defaults to the registry address and `NMOS_QUERY_PORT` to the registration port + 1. `NMOS_DNS_SD` defaults false. `true` exits 78. Nothing links Avahi or D-Bus. |
| G5 Announce IP addresses | met | `src/config.cpp:289`, `src/nmos.cpp:189` | `NMOS_HOST_ADDRESS` is the node href, API endpoint host and IS-05 control href. Unset, the first non-loopback IPv4 is used. Names, `0.0.0.0` and loopback are rejected. `HOST_ID` remains a seed alias only. |
| G6 Ports | met | `src/http_server.cpp`, `src/main.cpp:50` | `WEB_PORT` and `NMOS_PORT` are the only listeners. The UI WebSocket is on `WEB_PORT` (`/api/v1/events`). This node does not serve an IS-04 events socket, so there is no `NMOS_PORT+1` listener. A failed bind exits 75. |
| G7 Health and metrics | met | `src/app.cpp:286`, `src/app.cpp:304` | `/livez` is the process. `/readyz` is 200 only when serving and, with a registry configured, the Query API lists the node. Metrics use the prefix `mxl_test_player_`. |
| G8 Clean shutdown | met | `src/app.cpp:808`, `src/process.cpp` | SIGTERM stops ffmpeg children, releases writers, DELETEs the node, and with `MXL_CLEANUP_ON_EXIT=true` removes only the own domain. Exit 143, bounded by `SHUTDOWN_TIMEOUT_S` (default 10). `_uploads` is removed. |
| G9 IS-05 | met | `src/nmos.cpp` | Senders expose active `mxl_domain_id` and `mxl_flow_id`. `master_enable: false` stops that flow and the flags are restored from `CONFIG_DIR` after a restart. Receivers are N/A: this function has no inputs. |
| G10 Config export and import | met | `src/app.cpp:364` | `GET /api/v1/config/export` and `POST /api/v1/config/import`. No secrets exist. Import restores operator state and does not change deployment settings. |
| G11 Image and CI | met | `.github/workflows/container.yaml:40`, `docker/Dockerfile` | `main` publishes `git-<sha7>` and `nightly-dev`. A `vX.Y.Z` tag publishes `X.Y.Z`, `X.Y` and `X`. No `latest` tag. Runtime uid 1000. OCI labels include source, revision, licenses and `io.dmf.mxl.revision`. |
| G12 Kubernetes example | met | `deploy/k8s/deployment.yaml:18` | Pod network, standard env, `/livez` and `/readyz`, `terminationGracePeriodSeconds` 20, MXL hostPath, writable `/config`, uid 1000, `supplementalGroups: [1000]`, no `hostIPC`, no extra capabilities. |
| G13 Documentation | met | `README.md`, `CHANGELOG.md`, `SPECIFICATION.md` | Settings table, ports, exit codes, API list and the platform run notes. |
| G14 Tests | met | `tests/test_player.cpp:315`, `tests/integration/lifecycle.py` | Unit tests cover parsing, aliases, the announce address and domain creation. The lifecycle test covers ready, SIGTERM, DELETE and domain removal. |
