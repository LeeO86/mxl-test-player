# Changelog

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
