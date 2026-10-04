# mxl-test-player — Specification

Status: v1.0.0 stable platform contract. Draft notes below remain the functional spec; section 10 is the settings contract.

Changelog: v0.3 — live custom text burn-ins (§5.5) and a moving box with built-in box or any animated GIF/image as content (§5.6), editable live in the web UI. v0.2 — timecode as ANC flow (§7.1) and alpha/key output for stills (§7.2) are v1 requirements; candidate list updated (§12).
Repository: `LeeO86/mxl-test-player` (name can still change)
Aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`, `LeeO86/mxl-fabrics-agent`,
`LeeO86/mxl-multiviewer`, `LeeO86/mxl-webrtc-monitor`, `LeeO86/mxl-srt-gateway`,
platform repo `mmz-srf/mxl-poc-platform`. Functional inspiration: the CBC
`test-generator` and `file-player` from `cbcrc/mxl-hands-on`.

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-test-player` is an MXL source for tests and demos. Each of its **outputs**
writes one MXL video flow and one audio flow and is an NMOS sender pair, routable
by the crosspoint. An output plays one of:

- a **test pattern** (video) with **test signals** (audio),
- a **video file** from the media library, by default in an endless loop,
- a **still image** from the media library, with test audio or silence,
- a **playlist** of library items.

Uploaded media is **converted once at upload** into the platform's target format,
so playback is cheap, deterministic and gapless.

Design principles:

1. **Frame-accurate, TAI-locked output.** Every output writes exactly one grain per
   TAI grain index at its configured rate. Playback never drifts, stalls or skips.
2. **Heavy work at upload, light work at playback.** Transcoding, scaling,
   deinterlacing, loop alignment and audio conforming happen in a background
   conversion job; playback only decodes an intra-frame mezzanine or reads raw
   frames from RAM.
3. **Patterns that actually test something**: identification, field order, A/V
   sync, levels, motion and freeze detection.
4. **Conventions of the sibling repos**: config model, admin UI, `/metrics`, exit
   codes, CI, Compose and Kubernetes, uid 1000, MXL root `/Volumes/mxl`, pod network.

Out of scope for v1: live inputs, recording, compressed outputs, keying/graphics
overlays beyond §5.4, scheduling by time of day.

---

## 2. Architecture

```
 Web UI / REST ──► media library (upload ─► conversion job queue ─► mezzanine + metadata)
                    │
 per output:        ▼
   source = pattern generator | mezzanine decoder (decode-ahead) | RAM clip | image
        ─► burn-in overlay ─► TAI grain clock ─► MXL writers (v210, float32)
 nmos-cpp node: per output one video + one audio sender
```

- Each output has its own thread(s): a producer filling a bounded queue of ready
  frames and audio, and a writer driven by the TAI grain clock.
- If the producer is late, the writer repeats the last frame and counts an
  underrun (metric); this MUST NOT happen in normal operation (§10 targets).

---

## 3. Technology and build

- C++20, CMake ≥ 3.24, Ninja; GCC ≥ 12 or Clang ≥ 16.
- MXL `release/v1.1` at `218ddaa` (platform pin), Fabrics OFF; nmos-cpp at the
  siblings' commit.
- FFmpeg 7.x libraries (pinned) for conversion and mezzanine decode; image decode
  via FFmpeg (PNG, JPEG, TIFF, WebP) or a small image library — decide, record.
- Text rendering for burn-ins and identification: a proper font rasteriser
  (FreeType with a bundled font, e.g. DejaVu Sans / Inter) — not a bitmap font.
- Web UI: Vue 3 SPA embedded, no CDN; uploads via resumable chunked upload.
- GPU not required. Optional: NVDEC/NVENC are not used; CUDA not needed.
- Tests: doctest (vendored), shell integration tests.
- Record every deviation in `IMPLEMENTATION_PLAN.md`.

---

## 4. Formats

- **Platform format** (per container, `PLAYER_FORMAT`, default `1080p50`):
  raster 1920×1080, 1280×720 or 3840×2160; scan progressive or interlaced
  (1080i25, 1080i29.97 frames); rate 23.98–60. Optionally overridable per output
  (`outputs[n].format`) — then items are converted for each format they are used
  in (§6.3).
- Interlaced representation in MXL follows what mxl-decklink and
  mxl-st2110-gateway use for 1080i (grain = frame, field order in `flow_def.json`);
  verify against those repos and the pinned MXL.
- Video: `video/v210`, BT.709, SDR.
- Audio: `audio/float32`, 48 kHz, channel count per output (default 16,
  configurable 2–64); exact grain cadence (960 per grain at 50, 1601/1602 at 59.94).

---

## 5. Test patterns and signals

### 5.1 Video patterns

- Colour bars: SMPTE RP 219 (HD), EBU 100/75, 100/100, 75% bars; with optional
  PLUGE.
- Greyscale ramp (horizontal, vertical), luma steps, multiburst/frequency sweep,
  zone plate (static and moving), checkerboard, crosshatch/grid, circle (aspect
  check), safe-area markers, solid colours (black, white, red, green, blue, 50%
  grey).
- **Motion pattern** for freeze detection: a moving element (sweeping bar or
  rotating segment) that changes every frame.
- **Field-order pattern** (interlaced formats): an object moving in field-steps so
  wrong field order or wrong deinterlacing is immediately visible.
- **A/V sync pattern**: a white flash (full frame or marker) exactly on the frames
  where the audio carries a beep (§5.2), once per second by default; offset
  measurable in frames and ms.
- Patterns are generated at the output format natively (no scaling) and cached
  per format.

### 5.2 Audio signals

- Line-up tone per channel: sine, frequency and level per channel (default 1 kHz
  at −18 dBFS, the EBU R 68 alignment level).
- **Channel identification**: distinct frequency per channel, or the EBU stereo
  ident (left channel interrupted periodically), or voice-free "channel number as
  beep count" (n beeps for channel n, repeating).
- Pink noise, white noise, sweep (log), silence, polarity test (asymmetric pulse).
- **Sync beep** aligned to the A/V sync flash (§5.1).
- Per-channel mute, gain, invert.

### 5.3 Combinations

An output's "pattern" source is a pair (video pattern, audio signal set), saved as
named presets, e.g. "Bars + tone", "A/V sync", "Field order", "Ident 16 ch".

### 5.4 Burn-ins (all sources)

Configurable per output, rendered on top of any source:

- source label / output name (large, centred or corner);
- TAI-based timecode (HH:MM:SS:FF at the output rate) and/or UTC/local clock;
- frame counter (absolute grain index or since start);
- current item name and loop count;
- MXL flow ID (short) for identification in multiviewers.
- **Custom text** layers (§5.5).
- **Moving box** (§5.6).

All burn-ins are **live options**: they are switched and edited in the web UI
(and REST API) while the output is playing, take effect on the next grain, and
never interrupt, re-create or re-time the output flows. The current burn-in set of
an output is saved with the output's state and can be stored in presets.

### 5.5 Custom text

- Any number of text layers per output (up to 8), each with: text (UTF-8, multi-line),
  position (9 anchor presets plus x/y offset in % of the frame), font size (% of frame
  height), font (bundled fonts only), colour, opacity, optional background box
  (colour, opacity, padding), optional outline/shadow.
- Placeholders expanded every frame: `{label}`, `{timecode}`, `{tai}`, `{utc}`,
  `{local}`, `{frame}`, `{item}`, `{loop}`, `{host}`, `{flow}`; plain text otherwise.
- Editing the text in the UI updates the picture live (debounced, ≤ 100 ms after
  the last keystroke).
- Rendered with the FreeType text renderer (§3), anti-aliased; text layers are
  cached and only re-rendered when content or style changes.

### 5.6 Moving box

A moving element that proves the picture is live, usable on every source (pattern,
video, still, playlist):

- **Content**: a built-in box (solid colour with the frame counter inside, default)
  or any **image or animated GIF** from the media library (PNG/WebP with alpha,
  animated GIF/WebP/APNG). Transparency is respected.
- **Animated GIF conversion at upload**: frames are decoded, composited to full
  frames (GIF disposal methods), kept with alpha, scaled to a maximum sprite size,
  and stored as a frame sequence with per-frame durations. At playback the GIF
  frame shown is selected by elapsed TAI time, so its speed is independent of the
  output rate and loops endlessly.
- **Motion**: path `bounce` (DVD-style, default), `horizontal`, `vertical`,
  `circle`, `diagonal`; speed (% of frame width per second), size (% of frame
  height, aspect preserved), opacity, start position. Motion is computed from the
  TAI grain index, so it is deterministic and identical after a restart; a frozen
  output is therefore immediately visible.
- **Live control**: on/off, content picker (built-in box or library item), path,
  speed, size and opacity are changed live in the UI; the box continues from its
  current position (no jump) when parameters change.
- Up to 2 moving boxes per output.
- On keyed stills (§7.2) the moving box is drawn on fill only, unless "include in
  key" is enabled.

---

## 6. Media library and conversion

### 6.1 Upload

- Web UI upload (drag and drop, multiple files, chunked/resumable) and REST
  upload; optional import from a server directory (`PLAYER_IMPORT_DIR`, watched).
- Accepted inputs: anything FFmpeg can demux/decode (MOV, MP4, MXF, TS, MKV,
  ProRes, DNxHD/HR, H.264, HEVC, …), still images (PNG, JPEG, TIFF, WebP; alpha kept,
  §7.2) and animated images for the moving box (GIF, animated WebP, APNG; §5.6).
  Each item is typed at upload: `video`, `still`, or `sprite`.
- Per item metadata: name, tags, original file info (codec, raster, rate, scan,
  duration, audio layout), conversion status, thumbnail, conversion log.

### 6.2 Conversion job

Runs in a background queue (`PLAYER_CONVERT_CONCURRENCY`, default 1), with
progress in the UI. For video items:

1. **Video**: deinterlace/interlace, scale and frame-rate convert to the target
   format, with the same rules as mxl-srt-gateway §5.3 (bwdif for i→p,
   interlacing for p→i, aspect fit/fill with pillar-/letterbox selectable at
   upload, BT.601→BT.709). Frame-rate conversion by frame repeat/drop or, as an
   option at upload, by FFmpeg's motion-interpolating filter (slow; quality
   preview first).
2. **Audio**: resample to 48 kHz, map to the output channel count (default: keep
   source channels, pad with silence; mapping selectable at upload), loudness
   normalisation optional (EBU R 128, target −23 LUFS, off by default).
3. **Loop conforming**: trim to an integer number of frames; make audio length
   exactly match video length at 48 kHz for the target rate cadence, so that loops
   are seamless and A/V sync never drifts over loops; optional short audio
   crossfade at the loop point (default 0 ms, configurable).
4. **Mezzanine encoding** (§6.3) and thumbnail generation.

For images: decode, scale to the target raster (fit/fill/1:1 centred), convert to
v210 and store one raw frame.

Re-conversion: an item can be re-converted with other options; the original upload
is kept (configurable retention) so changing the platform format later is possible.

### 6.3 Mezzanine and RAM cache

- Default mezzanine: an intra-frame codec at the target format, decodable in real
  time on CPU. Candidates: ProRes 422 HQ (`prores_ks`), DNxHR HQX, FFV1 (lossless,
  larger). Default ProRes 422 HQ in MOV with PCM float audio; record the decision
  and measured decode speed per format.
- **RAM clip mode** for short items (≤ `PLAYER_RAM_CLIP_MAX_S`, default 20 s, and
  within a memory budget `PLAYER_RAM_BUDGET_MB`): the converted clip is decoded
  once into raw v210 + float32 in memory when the item is loaded on an output;
  playback is then a memory copy per grain. This guarantees gapless loops for
  stingers, idents and short clips.
- Storage layout under `PLAYER_LIBRARY_DIR` (default `/data/library`): one
  directory per item with `original.*`, `mezz-<format>.mov` or `frame-<format>.v210`,
  `thumb.jpg`, `item.json`.

---

## 7. Outputs and playback

- `PLAYER_OUTPUTS` outputs (default 2, max 16), each with label, format override,
  audio channel count, burn-in options, ANC timecode (§7.1), key mode (§7.2) and
  **current source**.
- Sources: pattern preset, video item, image item (+ audio signal set or silence),
  playlist.
- Transport per output: play, pause (holds frame, audio silence or held tone —
  configurable), stop (goes to the output's "idle source", default black + silence),
  restart, step frame (when paused), loop on/off (default on), seek (when not in
  RAM clip mode, frame-accurate to the nearest mezzanine frame).
- **Playlists**: ordered items with per-entry loop count (1..n or infinite for the
  last), cut transitions in v1; next item preloaded so the cut is frame-accurate.
- **Start on boot**: each output remembers its source and transport state and
  resumes after restart (state file under `/config`).
- **Gapless loop**: the next loop starts on the grain directly after the last frame;
  decode-ahead keeps at least `PLAYER_PREROLL_FRAMES` (default 25) ready.

### 7.1 Timecode as ANC flow (v1 requirement)

- Per output an optional `video/smpte291` flow (enabled by default) carrying
  SMPTE 12M timecode as ATC in ANC (SMPTE 12M-2, DID 0x60 / SDID 0x60), in the
  RFC 8331 payload layout used by mxl-decklink and mxl-st2110-gateway (verify the
  exact grain payload format against those repos and reuse it).
- Timecode source per output: `tai` (time of day from TAI at the output rate,
  default), `utc`/`local` time of day, `media` (timecode embedded in the item,
  falling back to 00:00:00:00 at item start), `free` (counts from a preset value).
- Drop-frame for 29.97/59.94 (configurable), field flag for interlaced formats;
  ATC_LTC by default, ATC_VITC1/2 optional.
- The same timecode drives the burn-in (§5.4), so picture and ANC always agree.
- NMOS: one data sender (`urn:x-nmos:format:data`, `video/smpte291`) per output in
  the output's group (`<output label>:Data`).

### 7.2 Alpha / key output for stills (v1 requirement)

- Image uploads keep their alpha channel (PNG, TIFF, WebP); conversion stores a
  fill frame and a key frame at the target format (premultiplied or straight,
  selectable at upload; default straight, as most keyers expect).
- Per output `key_mode`:
  - `off` (default): fill only, alpha ignored;
  - `fill_key`: two `video/v210` flows, **fill** and **key** (key as luma, full
    range 64–940 configurable, chroma at black), the usual fill/key pair for
    downstream keyers and vision mixers;
  - `v210a`: one `video/v210a` flow with embedded key — only if the pinned MXL and
    the receivers support it (check mxl-multiviewer and Strom; record the result).
- NMOS: in `fill_key` mode a second video sender `<output label>:Key` in the same
  group; both senders share timing exactly (same grain index, written together).
- Burn-ins (§5.4) are not drawn on keyed stills unless enabled explicitly.
- Video items and patterns have no key in v1; the key flow then carries full key
  (opaque) or no key (transparent), configurable (default opaque).

---

## 8. NMOS

- One Node, one Device; per output one video sender and one audio sender (plus
  data and key senders when those flows exist) with Source and Flow. Group hints
  stay `<output label>:Video` / `:Audio` / `:Data` / `:Key`. Every id is UUIDv5
  from `NMOS_SEED`.
- `NMOS_LABEL` is the node label and the device label. `NMOS_TAGS` (JSON object
  of name to string array) is added to the node and the device.
- Senders write into the player's own domain only. `domain_def.json` and
  `options.json` are written when the directory is created and are not rewritten
  on later starts. A different id already in `domain_def.json` is logged
  (`domain_id_mismatch`) and kept, not overwritten; NMOS announces that id.
- There are no receivers. IS-05 applies to senders: a staged PATCH with
  `master_enable` and `activate_immediate` starts or stops that flow.
  `master_enable: false` stops writing. The active enable flags are restored
  from the state file after a restart.
- Active IS-05 transport parameters are `mxl_domain_id` and `mxl_flow_id`.
- A format change of an output mints new flow IDs and updates IS-04 Flows and the
  senders' active params; source changes (pattern ↔ file) do **not** change flows.
- The IS-04 sender/flow labels SHOULD carry the output label, so multiviewer UMDs
  show it automatically.
- Static registry only. `NMOS_DNS_SD` defaults to false. `true` exits 78: this
  build does not browse or advertise DNS-SD and does not need Avahi or D-Bus.
- The node `href`, API endpoint host and IS-05 control href are the IPv4 address
  in `NMOS_HOST_ADDRESS` (default: the first non-loopback IPv4). They are never
  a hostname, `0.0.0.0` or a loopback address.
- This node does not serve an IS-04 events WebSocket. The UI WebSocket is
  `/api/v1/events` on `WEB_PORT`. `NMOS_PORT` is the node and connection API.

---

## 9. Web UI and API

- **Outputs**: one card per output with live thumbnail (low-rate JPEG), current
  source, transport buttons, loop toggle, progress bar and remaining time, audio
  meters (WebSocket), burn-in toggles, source picker (patterns, library, playlists).
- **Library**: upload, conversion queue with progress, item list with thumbnails,
  metadata, tags, filter/search, re-convert, delete (refused while in use).
- **Patterns**: presets editor (video pattern + audio signal set + parameters).
- **Burn-ins** panel per output (on the output card): toggles for the standard
  burn-ins, a list of text layers with inline editing, and the moving box controls
  with a sprite picker showing animated previews; all changes live (§5.4).
- **Playlists**: create/edit/reorder.
- **NMOS** and **Settings** pages like the siblings.
- REST under `/api/v1/…` (outputs, transport, library, uploads, jobs, presets,
  playlists, config, `config/export`, `config/import`), WebSocket `/api/v1/events`;
  `/livez`, `/readyz`, `/statusz`, `/metrics`.
- `GET /api/v1/config/export` returns version 1 JSON: informational deployment
  fields plus operator state (outputs, presets, playlists). There are no secrets.
  `POST /api/v1/config/import` restores that operator state. It does not change
  ports, the registry, or the seed.
- `/livez` is 200 while the process is running. `/readyz` is 200 when the
  process is serving and, if `NMOS_REGISTRY_ADDRESS` is set, the Query API lists
  this node. Otherwise `/readyz` is 503.
- Unauthenticated by design (lab network). Upload size limit configurable
  (default 20 GB); the platform ingress must allow it.

---

## 10. Configuration, metrics, deployment

Configuration (env > file > default; invalid exits 78):

| Key | Default |
| --- | --- |
| `PLAYER_FORMAT` | `1080p50` |
| `PLAYER_OUTPUTS` | 2 |
| `PLAYER_AUDIO_CHANNELS` | 16 |
| `PLAYER_LIBRARY_DIR` | `/data/library` |
| `PLAYER_IMPORT_DIR` | empty |
| `PLAYER_CONVERT_CONCURRENCY` | 1 |
| `PLAYER_RAM_CLIP_MAX_S` / `PLAYER_RAM_BUDGET_MB` | 20 / 4096 |
| `PLAYER_PREROLL_FRAMES` | 25 |
| `CONFIG_DIR` | `/config` (state, presets and playlists live here) |
| `PLAYER_CONFIG` | `<CONFIG_DIR>/player.json` (alias; `--config` wins) |
| `PLAYER_STATE` | `<CONFIG_DIR>/state.json` (alias of the state file path) |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` (parent of the default output domain; this player has no input flows to scan) |
| `MXL_OUTPUT_DOMAIN_DIR` / `MXL_OUTPUT_DOMAIN_ID` | `<scan>/player-<seed-short>` / UUIDv5 from the seed |
| `MXL_HISTORY_DURATION_NS` | `1000000000` (written into `options.json` only when that file is created) |
| `MXL_CLEANUP_ON_EXIT` | false (platform sets true; removes only this output domain) |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 |
| `NMOS_DNS_SD` | false (`true` exits 78) |
| `NMOS_PORT` | 3282 (node and IS-05; no second listener) |
| `NMOS_SEED` | `HOST_ID` + `-player` when unset. `HOST_ID` is only this seed alias, never an announced address |
| `NMOS_LABEL` | `MXL Test Player` |
| `NMOS_TAGS` | `{}` |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4. Rejects names, `0.0.0.0` and loopback |
| `SHUTDOWN_TIMEOUT_S` | 10 |
| `WEB_PORT` | 8130 (UI, REST, `/api/v1/events`, probes, `/metrics`) |

Unknown environment variables are ignored. A port that cannot be bound exits 75.
SIGTERM stops ffmpeg children immediately, releases MXL writers, DELETEs the
node from the registry, and with `MXL_CLEANUP_ON_EXIT=true` removes only this
output domain, then exits 143. The work is bounded by `SHUTDOWN_TIMEOUT_S`.
Operator state is only under `CONFIG_DIR`. Nothing secret is logged or exported.

Metrics (prefix `mxl_test_player_`): `output_state`, `grains_written_total`,
`underruns_total`, `loops_total`, `source_info` (info gauge), `decode_ahead_frames`,
`ram_clip_bytes`, `conversion_jobs` (by state), `conversion_duration_seconds`
(histogram), `library_items`, `library_bytes`, `upload_bytes_total`.
Grafana dashboard in `deploy/grafana/`.

Deployment: pod network; MXL root `hostPath`; library on a PVC (or hostPath) sized
for the media (documented); `/config` volume; uid 1000; CPU and memory requests
documented per output count and format. Compose demo (registry, two player
instances or one with two outputs, mxl-multiviewer and mxl-webrtc-monitor to view
them) and Kubernetes Deployment that `mxl-poc-platform` can vendor. CI and image
tags as the siblings (`ghcr.io/leeo86/mxl-test-player`), label
`io.dmf.mxl.revision`. Exit codes 0, 75, 78, 143.

Performance targets (record measured results): 4 outputs 1080p50 from ProRes
mezzanine on a Precision 3930 class CPU with zero underruns over 1 h; 2 outputs
2160p50 on an R740; RAM clip mode with 16 outputs 1080p50.

---

## 11. Testing

- Unit: pattern generators (reference checksums per format), audio signals
  (frequency/level via FFT, ident cadence), A/V sync alignment (flash frame index
  equals beep sample index / cadence), 59.94 audio cadence over 1000 grains,
  loop conforming (video frames × cadence = audio samples), playlist sequencing,
  config precedence, ID derivation, announce-address rejection, domain files
  created once, and child-process shutdown.
- Integration (CI, CPU): with a stand-in registry, `/readyz` stays 503 until the
  Query API lists the node, then 200; `config/export` round-trips through
  `config/import`; SIGTERM exits 143, the registry receives DELETE, the own
  domain and `_uploads` are removed.
- Integration (CI, CPU): upload a short 1080i50 H.264 clip with stereo AAC; the
  conversion produces a 1080p50 mezzanine with matching audio length; an output
  plays it in a loop for 3 loops: grain count equals TAI elapsed, no underruns,
  loop boundary has no audio gap (sample continuity check); switching to a
  pattern does not change the flow ID; changing the output format mints new flow
  IDs and updates NMOS.
- Integration: burn-ins change live without new flow IDs and without missed grains
  (grain count equals TAI elapsed while text and moving box are edited); the moving
  box position at a given grain index is identical before and after a restart; an
  animated GIF sprite plays at its own frame timing on 50p and 59.94p outputs.
- Integration: ANC timecode flow decodes (12M-2 ATC) to the same value as the
  burn-in for every grain over 60 s, including 29.97 drop-frame; a PNG with alpha
  in `fill_key` mode produces fill and key flows with identical grain indices and
  the expected key values at sampled pixels.
- NMOS conformance: AMWA IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01.

## 12. Candidate features (decide before or during implementation)

Decided for v1: timecode as ANC flow (§7.1), alpha/key output for stills (§7.2).
Not selected for v1, kept for later:

- Alpha/key for video items (e.g. ProRes 4444 with alpha).
- Mix/dissolve transitions in playlists.
- Scheduling (start item at a TAI/UTC time).
- External control via NMOS IS-12 or a simple TCP/HTTP "cart" protocol.
- Loudness measurement of library items shown in the UI (EBU R 128 report).
