# Changelog

## 1.1.0

- New web UI in the look of the other LeeO86 media functions (mxl-replay, mxl-multiviewer, mxl-st2110-gateway, mxl-browser-source): header with node label, format, playing, underrun, sender, conversion, registration and connection pills and the version; banners for a lost API, lost live updates and failed actions; tabs with `#hash` routing; light and dark theme. Every API function has a control:
  - **Outputs**: per output the picture, source, Play/Pause/Stop, Restart, Step, Loop, position with remaining time (click to seek a video), audio meters, counters, disabled senders, a preset in one click.
  - **Source**: source type (pattern, video, still, playlist), every video pattern, PLUGE, every audio signal with frequency, level and sync beep, *Save as preset*; output label, format, audio channels, key mode, idle key, ANC flow, timecode source, ATC kind, drop frame, audio while paused, loop. Drafts with Apply and Revert; a warning when the flows are opened again or get new ids.
  - **Burn-ins**: the standard burn-ins with the label position, up to 8 text layers (text with placeholder chips, position, offset, size, font, colour, opacity, box, outline, shadow) and up to 2 moving boxes (content from the library, path, speed, size, opacity, start, colour, in the key), live while typing.
  - **Library**: chunked upload (drop or pick, several files) with fit, frame-rate mode, alpha, loudness, crossfade, channel map and tags; conversions with progress and errors; items with thumbnail, original, conversions, options and the outputs using them; search, play on the selected output, edit name and tags, re-convert, delete.
  - **Patterns** (presets: apply, copy, edit as new, delete, take from an output), **Playlists** (create, rename, add, reorder, loops, hold time of stills, delete, play), **NMOS** (node, registration, every sender and flow, IS-05 master_enable on and off), **Status** (`/livez`, `/readyz`, `/statusz`, underruns of the last minute, read back the newest grain from MXL, library metrics), **Settings** (every setting with value and origin, export with copy and download, import).
  - Edits are drafts in the page: tab switches, status pushes and WebSocket reconnects do not overwrite them.
- New API, all additions: `GET /api/v1/info` (version, MXL revision, label, format, outputs); `GET /api/v1/config` lists `settings` with value and origin (`environment`, `file`, `argument`, `default`); `PATCH /api/v1/outputs/{n}` takes `master_video`, `master_audio`, `master_data`, `master_key`; the output status has `position`, `frames`, `pause_audio` and `idle_key`; `GET /api/v1/library/{id}/thumbnail`; `GET /api/v1/outputs/{n}/probe` without `index` reads the grain written two grains ago; `/api/v1/nmos` has `label`, `device_id`, `host_address`, `registered` and the Query API.

### Fixes

- Pause holds the frame on air. It showed the last seek or step point (the first frame after a play), so pausing a clip jumped back.
- The status `progress` and `remaining_s` follow the playhead (also within a playlist entry). `progress` stayed 0 while playing and `remaining_s` was the clip length.
- IS-05 `/active` `master_enable` follows the output, also after a restart or an output change (it went back to `true`), and a change over IS-05 is saved to the state file at once.
- A video that is still loading no longer counts a loop per frame.
- A playlist on an output comes back after a restart: the state file kept the source without its entries, so the output stayed black. The output status lists the source's `entries`.
- `PLAYER_CONFIG` and `CONFIG_DIR` choose the configuration file as documented; the process passed `/config/player.json` as if `--config` were given.
- The state file is written by one thread at a time.

## 1.0.4

- A new `domain_def.json` carries `description` and `tags`, as BCP-007-03 requires (`id`, `label`, `description`, `tags`). The player wrote only `id` and `label`, and mxl-st2110-gateway 1.0.2 skipped such domains. An existing file is still not rewritten.

## 1.0.3

Less CPU per output; the same grains as 1.0.2 (compared byte for byte on the lab for patterns and stills in all key modes). Lab host (2× Xeon Gold 6136, 60 s each): 4 × 1080p50 pattern 1.49 → 0.76 cores, with v210a 3.06 → 0.79, with fill_key 2.34 → 0.79; 16 × 1080p50 pattern 6.51 → 3.22 cores; 16 × 1080p50 RAM clip 6.01 → 4.69 cores; 2 × 2160p50 pattern 1.95 cores with 10 underruns → 0.73 cores with none.

- Progressive grains are rendered straight into the open MXL grain (fill, v210a alpha plane, fill_key key) instead of into a frame buffer that was then copied into the grain.
- A source that does not change (a still pattern, a still image, black, the idle key) is not copied again into a grain slot that already holds it: only the areas the burn-in drew on that slot last time are restored.
- Keyed outputs no longer allocate and fill new key and alpha planes for every grain.
- Burn-in: each colour is converted to YCbCr once instead of per pixel, and layers at full opacity blend in integers (the same values as before, unit test).
- Test tones keep an exact phase. `sin(2π·f·t)` with t from the TAI sample index lost precision (about 0.002 rad) and used most of libm's time; the phase is now reduced in integers and advanced by rotation.

### Fixes

- Restarting with `PLAYER_IMPORT_DIR` set imported every file there again: the library listed the item once more each time (`index.json` grew) and converted it again. The same file now keeps its one item; an index with repeated entries loads each item once.
- An output set to an item while the item was converting stayed black until the source was set again. Outputs waiting for their media now load it when the conversion finishes.

## 1.0.2

- An existing `domain_def.json` with another id than `MXL_OUTPUT_DOMAIN_ID` is logged (`domain_id_mismatch`) and kept, and NMOS announces that id. The player exited 78 before (platform guideline G2). A `domain_def.json` without an id still exits 78.
- The Kubernetes and Compose examples use the released image `1.0.2` instead of the moving `nightly-dev`, and the example's `io.dmf.mxl.revision` label is the MXL commit instead of `0.3.0`. The CMake project version is 1.0.2.

## 1.0.1

- NMOS registration sends complete IS-04 v1.3 resources (node `description` and `interfaces`, device `description`, source `caps` and audio `channels`, the flow's `flow_def.json`, sender `tags`). nmos-cpp rejected the previous bodies, so `/readyz` never turned 200 against it. Registry and Node API now use the same builders.
- Still test patterns are rendered once per writer thread and copied. A 1080p50 output took about 53 ms per grain and ran at about 19 fps; four outputs now keep 50 fps.
- The writer no longer fills and copies a key frame when keying is off and writes progressive grains without an extra copy.
- The writer thread reuses two frame buffers instead of allocating and zero-filling a new frame for every grain, and no longer encodes the thumbnail JPEG (every fifth grain, through a file): `GET .../thumbnail` encodes it from the latest frame when asked.
- Burn-ins are composited over the 6-pixel groups under each box instead of unpacking and packing the whole line for every row of the box. Same pixels as before.
- Measured on the lab host (2× Xeon Gold 6136, 120 s each): 2 × 2160p50 pattern outputs 32.2 → 49.7 grains/s per output; 16 × 1080p50 154 → 0 underruns at 8.6 → 6.9 cores; 4 × 1080p50 2.1 → 1.5 cores. Four 1080p50 outputs ran one hour on a quiet host with 2–4 underruns per output.
- The process raises its open-file soft limit to the hard limit at start. Each MXL flow keeps a descriptor per grain; with Docker's default of 1024, 16 outputs failed to open their flows and the process crashed.

## 1.0.0

Stable platform contract. A later breaking change of these settings, APIs or
behaviours needs v2.

- Configuration is environment, then one JSON file, then defaults. Invalid
  values exit 78. Operator state lives under `CONFIG_DIR` (default `/config`).
- New settings: `CONFIG_DIR`, `MXL_DOMAIN_SCAN_PATH`, `MXL_HISTORY_DURATION_NS`,
  `MXL_CLEANUP_ON_EXIT`, `NMOS_QUERY_ADDRESS`, `NMOS_QUERY_PORT`, `NMOS_LABEL`,
  `NMOS_TAGS`, `NMOS_HOST_ADDRESS`, `SHUTDOWN_TIMEOUT_S`.
- Aliases kept: `PLAYER_CONFIG`, `PLAYER_STATE`, and `HOST_ID` (seed prefix
  only, never an announced address).
- `NMOS_DNS_SD=true` now exits 78. This build has no DNS-SD browse and no mDNS
  advertisement. The default remains false.
- The node href, API endpoints and IS-05 control href announce
  `NMOS_HOST_ADDRESS` (or the first non-loopback IPv4). They no longer use the
  pod hostname.
- `/readyz` is 200 only when a configured registry's Query API lists this node.
- SIGTERM stops ffmpeg, releases MXL writers, DELETEs the NMOS node, removes
  the own domain when `MXL_CLEANUP_ON_EXIT=true`, and exits 143 within
  `SHUTDOWN_TIMEOUT_S`.
- `domain_def.json` and `options.json` are written once. A mismatched domain id
  is not overwritten.
- `GET /api/v1/config/export` and `POST /api/v1/config/import`. Nothing secret
  is stored. Import restores outputs, presets and playlists.
- Leftover `_uploads` directories are removed on shutdown and after a completed
  upload.
- Images: `git-<sha7>` and `nightly-dev` on `main`; `X.Y.Z`, `X.Y` and `X` on a
  `vX.Y.Z` tag. The moving `latest` tag is no longer published.
