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
  (1080i25, 1080i29.97 frames, top field first; `1080i25bff`, `1080i29.97bff`
  bottom field first); rate 23.98–60. Optionally overridable per output
  (`outputs[n].format`) — then items are converted for each format they are used
  in (§6.3).
- Interlaced representation in MXL: each grain is one field (not a frame), the
  field order is `interlace_mode` in `flow_def.json`, as in mxl-decklink and
  mxl-st2110-gateway with the pinned MXL:
  - Interlaced video and its ANC follow the shared MXL interlaced convention:
    https://github.com/LeeO86/mxl-multiviewer/blob/main/docs/mxl-interlaced.md
  - Pinned MXL `218ddaa` stores an interlaced flow as fields: each grain is one field
    of `height / 2` lines, at twice the definition's `grain_rate`. Grain 2k is the
    first field of frame k in time and grain 2k+1 the second (the writer requirement
    in mxl-multiviewer's specification): the first field is the top field (even
    lines) for `interlaced_tff` and the bottom field (odd lines) for `interlaced_bff`.
  - Each field is sampled at its own time: moving patterns (motion, moving zone
    plate, field order) and the moving boxes advance between the two fields of a
    frame; a converted item is made at the field rate and woven with
    `tinterlace` (`interleave_top` for tff, `interleave_bottom` for bff).
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
  grey), and **transparent** (1.3.0: black at alpha 0 in the `v210a` alpha plane and
  `key_min` in the `fill_key` key, whatever the idle key; a keyed output then carries
  only its burn-ins).
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
- Every burn-in (standard burn-ins, text layers, moving boxes, sprites) that is drawn
  on the fill is also drawn into the alpha of a keyed output (§7.2) with its own opacity
  (1.3.0; before, a moving box went into the `fill_key` key only with "include in key",
  and nothing went into the `v210a` alpha plane).

---

## 6. Media library and conversion

### 6.1 Upload

- Web UI upload (drag and drop, multiple files, chunked/resumable) and REST
  upload; optional import from a server directory (`PLAYER_IMPORT_DIR`, watched).
- Accepted inputs: anything FFmpeg can demux/decode (MOV, MP4, MXF, TS, MKV,
  ProRes, DNxHD/HR, H.264, HEVC, …), still images (PNG, JPEG, TIFF, WebP; alpha kept,
  §7.2) and animated images (GIF, APNG; §5.6, §6.2). Each item is typed at upload:
  `video`, `still`, or `sprite` (an animated image, also a single-frame GIF). Animated
  WebP needs an FFmpeg that decodes it (FFmpeg 8); the image's FFmpeg does not.
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
v210 and store one raw frame. Scaling is done premultiplied (1.3.0), so the colour of
transparent pixels does not bleed into the edges and the fill is black where the alpha is 0.

For animations (`sprite` items, 1.3.0): the file is decoded once per format, one frame at
a time (FFmpeg composites GIF and APNG frames with their disposal and transparency),
into both
- the moving-box sprite (§5.6): frames of at most `PLAYER_SPRITE_MAX_PX`, with their
  durations (`sprite.json`), written once (it does not depend on the format) into a
  temporary directory and renamed, `sprite.json` last, and
- a **clip with alpha** at the target format: each frame scaled like a still (`fit`),
  converted to a v210 fill and a 10-bit alpha plane (`alpha_mode` as for stills) and
  appended to `anim-<format>.v210.part` and `.a10.part`, renamed over the clip at the end.
GIF delays of 10 ms or less are 100 ms, as browsers play them.
Bounds: the frames are counted from the packets first; more than 1000 frames, more
pixels than 8192×4608 per frame, or more pixels to decode in all than
`PLAYER_RAM_BUDGET_MB` holds in RGBA (frames × width × height × 4) fails the conversion
with that reason; a sprite that does not fit the budget fails; a clip that does not fit
it (frames × the larger of fill + alpha and fill + key at the format) is skipped with
`clip: false` and `clip_error`, the sprite stays usable. A failed conversion is queued
again only by a re-convert, not by playing the item. Disk: the clip is raw, about 8 MB per animation frame at 1080 lines
(33 MB at 2160). The conversion records `frames` (animation frames), `seconds` (one
loop), `clip` and `clip_frames` (output frames per loop). A sprite converted before
1.3.0 is converted once more the first time it is played as a source.

Re-conversion: an item can be re-converted with other options; the original upload
is kept (configurable retention) so changing the platform format later is possible.

### 6.3 Mezzanine and RAM cache

- Default mezzanine: an intra-frame codec at the target format, decodable in real
  time on CPU. Candidates: ProRes 422 HQ (`prores_ks`), DNxHR HQX, FFV1 (lossless,
  larger). Default ProRes 422 HQ in MOV with PCM float audio; record the decision
  and measured decode speed per format.
- **RAM clip mode** for short items (≤ `PLAYER_RAM_CLIP_MAX_S`, default 20 s, and
  within the memory budget `PLAYER_RAM_BUDGET_MB`, one budget for the whole process
  since 1.3.0: every output's RAM clips count, an output's old and new clip both while
  it switches; a video that does not fit streams from its mezzanine): the converted clip is decoded
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
  playlist. An animated `sprite` item (1.3.0) plays as a source of type `video` (or
  `still`), fired like any clip (`PUT …/source`, then `POST …/transport`): it is read
  into RAM once and shared by every output that plays it (the fill, the `v210a` alpha,
  and the `fill_key` key per key range, made from the alpha at load), within
  `PLAYER_RAM_BUDGET_MB`; each output frame shows the animation frame of its time. A
  playlist entry set through the API needs `frames` = the item's `clip_frames`. One loop
  is the animation's duration rounded to whole output frames (at least 1), so it loops
  seamlessly, seeks, pauses and plays in playlists like a video; its speed is off by at
  most half an output frame per loop. Both fields of an interlaced frame show the same
  animation frame. It has no audio (silence as a `video`, the test signal as a
  `still`) and is a keyed source: its alpha goes to the key (§7.2) and the standard
  burn-ins and text stay off unless `on_keyed_stills`. A one-frame animation is a still
  with alpha. An item that cannot play says why in the output status `media_error`.
  While an item with alpha (a still, an animation) is loading, converting or refused, a
  keyed output is transparent (alpha 0, `key_min`), not the idle key; stop shows the
  idle key. Media an output no longer plays is freed by its loader thread, not by the
  real-time writer.
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
- Grain layout (1.2.4), as MXL documents it (docs/Architecture.md, `video/smpte291`):
  the RFC 8331 payload from the Length field on, without the RTP header and the
  extended sequence number: Length (16 bits) at byte 0, ANC_Count (8), F (2),
  reserved (22), then the ATC packet at byte 6. The rest of the 4096-byte grain is
  zero.
- Timecode source per output: `tai` (time of day from TAI at the output rate,
  default), `utc`/`local` time of day, `media` (timecode embedded in the item,
  falling back to 00:00:00:00 at item start), `free` (counts from a preset value).
- Drop-frame for 29.97/59.94 (configurable), field flag for interlaced formats;
  ATC_LTC by default, ATC_VITC1/2 optional.
- Interlaced formats (1.2.3): the data flow runs at the field rate, as the shared
  interlaced convention requires: `grain_rate` twice the frame rate (50/1 at 1080i25,
  60000/1001 at 1080i29.97); grain 2k carries the first field's packet (RFC 8331
  F = 0b10, line 10) and grain 2k+1 the second field's (F = 0b11, line 572, field
  flag set), at the same index as the video field. Both carry the frame's timecode.
- The same timecode drives the burn-in (§5.4), so picture and ANC always agree.
- NMOS: one data sender (`urn:x-nmos:format:data`, `video/smpte291`) per output in
  the output's group (`<output label>:Data`).

### 7.2 Alpha / key output for stills (v1 requirement)

- Image uploads keep their alpha channel (PNG, TIFF, WebP, and GIF/APNG played as a
  clip, §7); conversion stores a
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
- Burn-ins composite "over" the alpha (1.3.0): `a_out = a_src + a_dst · (1 − a_src)`, in the
  `v210a` alpha plane and in the `fill_key` key (at `key_min`..`key_max`). The fill stays
  straight: `(c_src · a_src + c_dst · a_dst · (1 − a_src)) / a_out`, so a burn-in over a
  transparent background keeps its own colour; over a premultiplied still it is
  `c_src · a_src + c_dst · (1 − a_src)`. 4:2:2 chroma is blended once per pixel pair.
- Video items and patterns have no key in v1; the key flow then carries full key
  (opaque) or no key (transparent), configurable (default opaque). Since 1.3.0 the
  `v210a` alpha plane follows the same setting (it was always 0). An output that feeds a
  keyer with graphics should set `idle_key: transparent`.

---

## 8. NMOS

- One Node, one Device; per output one video sender and one audio sender (plus
  data and key senders when those flows exist) with Source and Flow. Group hints
  stay `<output label>:Video` / `:Audio` / `:Data` / `:Key`. Every id is UUIDv5
  from `NMOS_SEED`; sender, source and flow ids also from the output's position
  (index in the configuration), so reordering outputs changes them. Two players
  must not share a seed: they would share node and device ids and DELETE each
  other's resources. Without `NMOS_SEED` and `HOST_ID` the seed is a random
  `player-<16 hex>` kept in `CONFIG_DIR/nmos_seed` (the hostname when that is not
  writable), with a warning at start.
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
- Sender and Source ids depend only on the seed, the output and the role
  (video, audio, data, key), not on the format, the video media type (`v210` /
  `v210a`) or the channel count. A format change keeps the sender: its `flow_id`
  and IS-05 `mxl_flow_id` move to the new flow, and the old Flow is DELETEd from
  the registry (senders first, then flows, then sources). An output that drops
  its ANC or key flow DELETEs that sender, flow and source. The registry is
  updated at once, not at the next 5 s heartbeat. Releasing the old MXL writer
  removes the old flow from the own domain. After a run that ended without the
  node DELETE, the next start reads the device's resources from the Query API
  and DELETEs the ones it no longer offers.
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
- 1.1.0 implements this in the look of the sibling UIs (header pills, banners, `#hash` tabs, light and dark
  theme): Outputs, Source (source and output settings), Burn-ins, Library, Patterns, Playlists, NMOS (with
  each sender's IS-05 `master_enable`), Status (probes, underruns, read-back of the newest grain) and Settings
  (settings with their origin, export, import). Edits are drafts that survive tab switches and reconnects.
- REST under `/api/v1/…` (outputs, transport, library, uploads, jobs, presets,
  playlists, config, `config/export`, `config/import`), WebSocket `/api/v1/events`;
  `/livez`, `/readyz`, `/statusz`, `/metrics`.
- Additions for the UI (1.1.0): `GET /api/v1/info` (version, MXL revision, label, format, outputs);
  `settings` in `GET /api/v1/config` (key, value, origin); `master_video|audio|data|key` in
  `PATCH /api/v1/outputs/{n}` (the senders' IS-05 `master_enable`); `position`, `frames`, `pause_audio` and
  `idle_key` in the output status; `GET /api/v1/library/{id}/thumbnail`; `probe` without `index` reads the
  newest grain; `/api/v1/nmos` with label, device id, address and registration state.
- `GET /api/v1/config/export` returns version 1 JSON: informational deployment
  fields plus operator state (outputs, presets, playlists). There are no secrets.
  `POST /api/v1/config/import` restores that operator state. It does not change
  ports, the registry, or the seed.
- `/livez` is 200 while the process is running. `/readyz` is 200 when the
  process is serving and, if `NMOS_REGISTRY_ADDRESS` is set, the Query API lists
  this node. Otherwise `/readyz` is 503.
- Unauthenticated by design (lab network). Upload size limit configurable
  (default 20 GB); the platform ingress must allow it.

### 9.1 Widgets (1.2.0, operator screens)

The contract of mxl-webrtc-monitor 1.3.0, mxl-replay 1.4.0 and mxl-color-corrector 1.1.0.

- `GET /widgets` answers `[{id, title, params, min_size: {w, h}, version}]` (`params` a
  JSON schema of the query). It carries `Access-Control-Allow-Origin` for an `Origin`
  that `WIDGET_FRAME_ANCESTORS` lists (or `*`), and `Vary: Origin`, instead of the API's
  `*`; GET only (405 otherwise).
- `transport` (`output`, required, 1..`PLAYER_OUTPUTS`: 1 is the first output, API index 0;
  `min_size` 420×160): one output's label, format and transport state, what it plays
  (pattern and test audio, item, or playlist and item), Play/Pause/Stop, Restart, Step,
  Loop, and the position with remaining time (a click seeks a video). The source, burn-ins
  and output settings stay in the app.
- `GET /widget/transport?output=<n>[&theme=dark|light|transparent]` is the UI's `index.html`
  with only that widget, no app chrome, on the player's own API (same origin, WebSocket
  `/api/v1/events` included). An invalid parameter answers 400, an unknown widget 404. These
  routes carry `Content-Security-Policy: frame-ancestors <WIDGET_FRAME_ANCESTORS>` and no
  `X-Frame-Options`.
- The page posts `{type: "widget-ready"}` once it shows the output, and
  `{type: "widget-size", w, h}` then and on every resize, to `window.parent`.
- No video preview: the pictures are JPEG thumbnails, so the platform's preview contract
  (`PREVIEW_*`) does not apply.

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
| `NMOS_SEED` | `HOST_ID` + `-player` when unset. `HOST_ID` is only this seed alias, never an announced address. Without both: a random seed kept in `CONFIG_DIR/nmos_seed`, else the hostname, with a warning |
| `NMOS_LABEL` | `MXL Test Player` |
| `NMOS_TAGS` | `{}` |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4. Rejects names, `0.0.0.0` and loopback |
| `SHUTDOWN_TIMEOUT_S` | 10 |
| `WEB_PORT` | 8130 (UI, REST, `/api/v1/events`, probes, `/metrics`, widgets) |
| `WIDGET_FRAME_ANCESTORS` | `'self'`: CSP `frame-ancestors` of the widget pages (§9.1). `;`, `,` or control characters exit 78 |

Unknown environment variables are ignored. A port that cannot be bound exits 75.
SIGTERM stops ffmpeg children immediately, releases MXL writers, DELETEs the
node from the registry, and with `MXL_CLEANUP_ON_EXIT=true` removes only this
output domain, then exits 143. The work is bounded by `SHUTDOWN_TIMEOUT_S`.
Operator state is only under `CONFIG_DIR`. Nothing secret is logged or exported.

Metrics (prefix `mxl_test_player_`): `output_state`, `grains_written_total`,
`underruns_total`, `loops_total`, `source_info` (info gauge), `decode_ahead_frames`,
`ram_clip_bytes`, `ram_budget_used_bytes` (1.3.0, the process-wide budget), `conversion_jobs` (by state), `conversion_duration_seconds`
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
- Integration (CI, CPU): with a stand-in registry, switching an output from v210
  to v210a, its format, ANC and key leaves exactly one sender per output and role,
  on the new flow; the old flow (and a dropped role's sender and source) is
  DELETEd from the registry and the old MXL flow is gone; a restart after SIGKILL
  with another format removes the flows the old run left.
- Integration: burn-ins change live without new flow IDs and without missed grains
  (grain count equals TAI elapsed while text and moving box are edited); the moving
  box position at a given grain index is identical before and after a restart; an
  animated GIF sprite plays at its own frame timing on 50p and 59.94p outputs.
- Integration (1.3.0, `tests/integration/keying.py`, reading the grains back with
  `player_grain_dump`): the alpha (`v210a`) and the key (`fill_key`) under a moving box
  and a text box, the transparent pattern, an animated GIF clip (alpha 1023 under its
  square, 0 around, the right GIF frame at a seeked output frame) and a 50 % APNG.
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
