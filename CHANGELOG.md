# Changelog

## Unreleased

- NMOS registration sends complete IS-04 v1.3 resources (node `description` and `interfaces`, device `description`, source `caps` and audio `channels`, the flow's `flow_def.json`, sender `tags`). nmos-cpp rejected the previous bodies, so `/readyz` never turned 200 against it. Registry and Node API now use the same builders.
- Still test patterns are rendered once per writer thread and copied. A 1080p50 output took about 53 ms per grain and ran at about 19 fps; four outputs now keep 50 fps.
- The writer no longer fills and copies a key frame when keying is off and writes progressive grains without an extra copy.
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
