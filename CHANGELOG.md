# Changelog

## 1.3.1 (2026-10-30)

### Fixes

- **The library no longer loses items it has on disk.** Items whose folders and `item.json` were on disk could be missing from the library after a restart, silently. Three causes, all fixed:
  - `index.json` was rewritten in place, and a failed write was ignored: when it could not be written (for example a file the player's user may not write), the running player still listed every item, but the file kept an older list, and the next start loaded only that. The first change after it (a conversion, an upload) then wrote the shorter list, so the items stayed lost. A write that stops halfway (the process killed) left a cut-off file, and the next start loaded no item at all. Now `index.json` and `item.json` are written to a temporary file and renamed over the old one; a failed write is logged (`ERROR library: …`).
  - An `index.json` entry with a field of the wrong type stopped the start with an exception. It is now logged and skipped; the other entries load.
  - **At start, every item folder whose `item.json` is not in `index.json` is restored from it**, logged as `WARN library: restored <id> (<name>) from its item.json`, and `index.json` is written again. This also brings back items lost before 1.3.1 (their folders are still there), and items dropped when an older `index.json` is put back from outside (a backup or a state restore of the library volume). The start logs `INFO library: N items (M restored from item.json)`.
- A deleted item removes its `item.json` first, so a folder that cannot be removed completely does not come back at the next start.
- `deploy/k8s/deployment.yaml` sets `strategy: Recreate`: with the default rolling update the old and the new pod ran on the same library at once, and the last one to write `index.json` dropped the items the other had added.

## 1.3.0 (2026-10-30)

### Breaking

- **The `v210a` alpha plane of patterns, videos and black now follows `idle_key`** (default `opaque`, 1023; it was 0). An output that feeds a keyer with graphics must set `idle_key: transparent`, or the keyer sees an opaque frame under the graphics.
- **`include_in_key` of the moving box is ignored**: every moving box (and every other burn-in) is in the key.

### New

- **An animated GIF or APNG plays as a clip with its alpha.** Before, an uploaded GIF was only a moving-box sprite. A `sprite` item now also plays as a source (`PUT /api/v1/outputs/{n}/source` with `{"type": "video", "item_id": …}`, then `POST …/transport`, as any clip is fired; also in playlists, and from the web UI's Source tab, Library *Play on* and Playlists): it loops at its own frame timing, mapped onto the output rate (one loop is the animation's duration rounded to whole output frames, so it loops seamlessly and seeks, pauses and steps like a video; at most half an output frame per loop off). Its alpha goes into the `v210a` alpha plane or the `fill_key` key, and burn-ins composite over it. It has no audio. A one-frame GIF plays as a still with alpha. The moving-box use is unchanged. A GIF delay of 10 ms or less plays as 100 ms, as in browsers.
  - A playlist entry set through the REST API needs `frames` = the item's `clip_frames` at the output's format (the web UI does this).
  - Converted once per format at upload, frame by frame (never decoded per grain): the sprite frames (written once, not per format), plus a raw clip at the format (`anim-<format>.v210` and `.a10`, replaced by rename). **Disk: about 8 MB per GIF frame per format at 1080 lines** (33 MB at 2160); a 40-frame GIF of 277 KB takes 316 MB at 1080p50. A sprite uploaded before 1.3.0 is converted once more the first time it is played.
  - Played from RAM. **`PLAYER_RAM_BUDGET_MB` is now one budget for the whole process** (it was checked per clip and per output): every output's RAM video clips and animation clips count against it, the old and the new clip both while an output switches. A clip's fill and alpha (and a `fill_key` key per key range) are loaded once and shared by every output that plays it. A clip that does not fit is not played (`media_error` says why); a video that does not fit streams from its mezzanine as before. New metric `mxl_test_player_ram_budget_used_bytes`; `mxl_test_player_ram_clip_bytes{output}` counts what each output holds (shared data at each output).
  - Bounded: frames are counted from the packets before anything is decoded. More than 1000 frames, more pixels than 8192×4608 per frame, or more pixels to decode in all than the budget holds in RGBA (frames × width × height × 4 bytes) fails the conversion with that reason. A clip that does not fit the budget (frames × the larger of fill + alpha and fill + key) is skipped with `clip: false` and a `clip_error`; the item stays usable as a sprite. One frame is held at a time while converting. A failed conversion is not queued again by playing the item, only by a re-convert; the job list keeps the last 100 finished jobs.
  - New fields of an animation's conversion: `clip`, `clip_frames` (output frames per loop), `clip_error`, `seconds`. New field `media_error` in the output status: why the item cannot play.
  - **While an item with alpha (a still, an animation) is loading, converting or refused, a keyed output is transparent** (alpha 0, `key_min`), not the idle key. Stop shows the idle key. **Outputs that feed a keyer with graphics should set `idle_key: transparent`.**
  - Animated WebP is not decoded by the image's FFmpeg (6.1); static WebP is.
- **New pattern `transparent`**: black at alpha 0 (`v210a`) or `key_min` (`fill_key`), whatever the idle key, so a keyed output can carry only its burn-ins, or a GIF clip over nothing.
- Images and animations are scaled premultiplied: the colour of transparent pixels (white in many GIFs) no longer bleeds into the edges, and the fill is black where the alpha is 0. This also applies to stills converted from now on.
- An upload is typed with libavformat when `ffprobe` gives nothing.
- Old media is freed by the output's loader thread, never by its real-time writer (freeing a 400 MB clip took longer than a frame).
- Lab (1080p50, three outputs `v210a`, `fill_key`, `off`, a 480×480 GIF of 40 frames at 25 fps fitted to 1920×1080): conversion 2.5 s, 316 MB on disk; load 0.3, 0.8 and 0.01 s (the third output finds it shared); process CPU 32 % for the three outputs playing it (45 % for bars with the default burn-ins, 76 % with the GIF clip, every burn-in and the GIF as a moving box); budget used 527 MB for the three outputs (316, 421 and 210 MB per output before sharing); RSS 1.9 GB (2.3 GB without sharing); no underruns. Switching all three outputs between two such GIFs once a second for 30 s: no underruns.

### Fixes

- **Switching to a pattern no longer drops frames.** A pattern that does not move was rendered in the output's real-time writer the first time it showed after a switch (the writer kept one pattern), which took longer than a frame: switching three 1080p50 outputs between bars and a grey ramp once a second gave 14 underruns per output in 30 s. The loader thread now renders a newly selected pattern (and its A/V-sync flash frame), and the writer only takes the ready frame; until then it keeps the last pattern (or black). Same test: 0 underruns. Moving patterns are rendered per grain as before.
- **Burn-ins go into the key.** On a `v210a` output the alpha plane was never drawn on, so every burn-in (moving box, sprite, text, the standard burn-ins) was in the fill but vanished in a downstream keyer; on a `fill_key` output only a moving box with "include in key" went into the key, text never. Now every burn-in drawn on the fill is also drawn into the alpha (the `v210a` alpha plane, or the `fill_key` key at `key_min`..`key_max`) with its own opacity, composited "over": `a_out = a_src + a_dst · (1 − a_src)`. The fill stays straight alpha (`(c_src · a_src + c_dst · a_dst · (1 − a_src)) / a_out`), so a burn-in over a transparent background has its own colour, not a darkened one; over a still converted as `premultiplied` the fill is blended premultiplied. Outputs without a key (`key_mode` `off`) are drawn exactly as before.
- **The moving box's `include_in_key` is gone** (API, state file, web UI checkbox "In the key"): every box is in the key now. The field is still accepted and ignored, so saved states and older clients keep working.
- **The `v210a` alpha plane of patterns, videos and black follows `idle_key`** (default `opaque`: 1023), as the `fill_key` key always did. It was 0 (transparent) whatever `idle_key` said.
- New unit tests: the over-composite of alpha and straight fill against the formula (transparent, half and opaque background), the key range of `fill_key`, the premultiplied blend, nothing outside the layer, restoring the alpha plane under the overlay, and a text layer and a moving box writing their opacity into a `v210a` plane.
- New integration test `tests/integration/keying.py` (also in CI): reads the MXL grains back with the new lab tool `build/player_grain_dump` and checks the alpha (`v210a`) and the key (`fill_key`) under a 60 % moving box, a 50 % text box and the transparent background, the `transparent` pattern, a GIF clip (alpha 1023 under its square and 0 around, the right GIF frame at a seeked output frame, the key in `fill_key`) and a 50 % APNG; `--shots DIR` writes the fill and the key as PNG.
- New unit tests: the animation timing (loop length at 25, 50 and 59.94, even and uneven GIF delays), and the conversion of a GIF into a sprite and a clip with alpha (black fill where transparent), a clip over the RAM budget refused with a reason, an animation of 1001 frames and one with too many pixels to decode refused, and a failed conversion not queued again. The test GIFs are written by the test itself (the image build has no ffmpeg program).
- `keying.py` also checks that a GIF whose clip was refused leaves a keyed output transparent with the idle key opaque, and that stop then shows the idle key.

## 1.2.4

### Fixes

- **The ANC grain starts at the RFC 8331 Length field, as MXL stores it.** MXL (docs/Architecture.md, `video/smpte291`) keeps the RFC 8331 payload from the Length field on; the RTP header and the extended sequence number are not stored. The player wrote a 2-byte sequence number first, so Length was at byte 2 and the ATC packet at byte 8, and a reader that follows MXL (mxl-st2110-gateway ANC egress, MXL's `mxl-data-probe`, gst-mxl-rs) read the sequence number as Length and the wrong bytes as the packet. Now Length is at byte 0, ANC_Count at byte 2, the F bits at byte 3 (unchanged values: 0b00 progressive, 0b10 field 1, 0b11 field 2), reserved to byte 5, and the packet at byte 6. The packet itself (line, horizontal offset, DID/SDID 0x60/0x60, UDW, checksum) is unchanged. The sequence number is gone.
- The status probe (`GET /api/v1/outputs/{id}/probe`) reads the new layout. Grains written by an older version (a stale data flow left in the domain) are no longer decoded there.
- The status probe no longer reads past its buffer. When the first timecode decode failed, it decoded again with a fixed size of 4096 bytes, though the buffer holds only the grain's first min(grain size, 4096) bytes; a data grain smaller than 4096 bytes would have been read out of bounds (MXL's ANC grains are 4096 bytes, so the second decode was otherwise the same as the first). It now decodes once, bounded by the buffer's real size.
- New unit test: the grain bytes of a progressive LTC packet and of a field-2 VITC1 packet equal reference vectors from mxl-st2110-gateway's ANC codec (`codec::serialiseGrain`). Checked once against the gateway's parser (`codec::parseGrain`): it reads both grains with no parity or checksum errors and re-serialises them byte for byte.
- The Compose and Kubernetes examples use the `1.2.4` image.

## 1.2.3

### Fixes

- **The ANC timecode flow of an interlaced format runs at the field rate.** At `1080i25` and `1080i29.97` (tff and bff) the `video/smpte291` data flow was declared at the frame rate (25/1, 30000/1001) but written at the video's field index, so its grain index was about twice the TAI index of its own rate, and a reader that asks for the TAI index of the declared rate (mxl-decklink playout ANC, mxl-st2110-gateway ANC egress) got nothing or the wrong grain. Following the shared interlaced convention (MXL 218ddaa), the data flow is now declared at twice the frame rate (`grain_rate` 50/1 at 1080i25, 60000/1001 at 1080i29.97, in `flow_def.json` and the NMOS flow), and grain 2k carries the first field's ATC packet and 2k+1 the second field's, at the same index as the video field. Progressive formats are unchanged.
- **The second field's packet has a second-field line number.** Each field's packet had RFC 8331 line 10; the second field's is now line 572 (line 10 of field 2 in the 1125-line raster, SMPTE ST 274). Unchanged: both fields carry the frame's timecode, the second with the field flag set, the RFC 8331 field bits `F` are 0b10 on the first field and 0b11 on the second, and the sequence number is the grain index (low 16 bits), which now counts the data flow's own grains.
- Lab (1080i25, one output): the data flow's head equals `mxlGetCurrentIndex(50/1)`; grain 2k has F 0b10 and line 10, grain 2k+1 F 0b11 and line 572; the NMOS data flow says 50/1.
- New unit test: the data flow's rate per format, and the field bits, line, sequence and timecode of the packets of grains 2k and 2k+1 (and of a progressive grain).
- **Upgrade:** a data flow left in the domain by an older version that did not exit cleanly (SIGKILL, a crash) keeps its 25/1 definition, because MXL opens an existing flow as it is. Remove the output domain (or that flow) once if the flow still says 25/1 after the upgrade.
- The Compose and Kubernetes examples use the `1.2.3` image.

## 1.2.2

- New formats for bottom field first: `1080i25bff` and `1080i29.97bff` (`PLAYER_FORMAT`, an output's `format`, the web UI's format list). `1080i25` and `1080i29.97` stay top field first (also accepted as `1080i25tff`, same name and flow ids). Before, every interlaced format was tff.

### Fixes

- **Bottom field first is written in the right lines.** Pinned MXL `218ddaa` stores an interlaced flow as fields (one grain = one field of `height / 2` lines, at the field rate). The writer took a grain's lines from its index alone (grain 2k the even lines, 2k+1 the odd lines), which is right for `interlaced_tff` only: an `interlaced_bff` flow carried each field one line off. Grain 2k is now the first field of frame k in time, as the multiviewer's specification requires of writers: the top field (even lines) for tff, the bottom field (odd lines) for bff. The fill, the v210a alpha plane and the `fill_key` key all follow it.
- **Each field is sampled at its own time.** Moving patterns were rendered once per frame and both fields cut from that frame, so an interlaced output showed no motion between its two fields. The motion pattern, the moving zone plate and the field-order pattern are now rendered for each field's time (the second field half a frame later); the field-order pattern's bar moves one step per field and its top stripe is green on the first field and red on the second. At interlaced formats the motion pattern's bar now moves one pixel per field (it moved one per frame). Moving boxes and text burn-ins already followed the grain.
- At every interlaced format (tff too), moving patterns now change between the two fields of a frame: a paused motion or field-order pattern alternates between two field times, and the field-order pattern's stripe is red on the second field.
- A video item converted for a bff format is woven with the earlier picture in the bottom field (`tinterlace=interleave_bottom`; tff keeps `interleave_top`).
- New unit tests: the tff and bff formats, the lines of each grain, a reader that weaves the field grains as the multiviewer does (grain 2k is the first field) and gets every line back in place, and the field-order pattern moving forward from the first field to the second and on to the next frame.
- The Compose and Kubernetes examples use the `1.2.2` image.

## 1.2.1

### Fixes

- An output change no longer leaves a stale sender in the NMOS registry. After Out 2 went from `v210` to `v210a`, the registry listed two "Out 2 Video" senders: the new one and the old one on the old flow. The registration loop only POSTed what the node offered and never DELETEd what it stopped offering, and the sender id was derived from the flow id, which includes the format.
  - Sender ids no longer include the format, the video media type or the channel count: UUIDv5 of the node and `output/<n>/sender/<role>`. A format change keeps the sender, which points at the new flow (IS-04 `flow_id`, IS-05 `mxl_flow_id`); controllers keep their sender.
  - **Every sender id changes once with 1.2.1** (it was derived from the flow id). Routes and presets stored by sender id must be picked again once after the upgrade.
  - **After a format change, receivers must be activated again** with the sender's new `mxl_flow_id`: the old MXL flow is removed, so a receiver still on the old flow id gets no more grains. The sender id stays, so a controller re-activates the same sender.
  - The node remembers what it registered and DELETEs what the model no longer has: the old flow after a format, `v210a` or channel change; sender, flow and source when an output drops its ANC or key flow. Senders go first, then flows, then sources.
  - An output change, and an IS-05 `master_enable` change (`subscription.active`), is registered at once, not at the next 5 s heartbeat.
  - After a run that ended without the node DELETE (a crash, SIGKILL), the first registration reads the device's sources, flows and senders from the Query API and DELETEs the ones it no longer offers. This is tried again until all three lists came back. A DELETE that fails is retried at every pass and warned once a minute.
  - The old MXL flow was already removed when its writer is released (the player is its only writer); the new integration test checks it.
- Without `NMOS_SEED` and `HOST_ID`, the seed was `host-player` on every machine, so such players shared node and device ids (and with the cleanup above would remove each other's resources). The seed is now a random `player-<16 hex>`, kept in `CONFIG_DIR/nmos_seed` so the ids survive a restart, or the hostname when `CONFIG_DIR` is not writable; a warning at start says so. Set `NMOS_SEED` in production. The docs now say that sender, source and flow ids also depend on the output's position in the configuration.
- The web UI's warning for a change that mints new flow ids says that the senders keep theirs.
- New integration test `tests/integration/stale_sender.py` with a stand-in registry (`tests/integration/registry_stub.py`, also runnable on its own for lab checks).
- The Compose and Kubernetes examples use the `1.2.1` image.

## 1.2.0

Operator-screen widgets, as in mxl-webrtc-monitor 1.3.0, mxl-replay 1.4.0 and mxl-color-corrector 1.1.0 (`SPECIFICATION.md` §9.1):

- `GET /widgets` lists one widget, `transport` (`output`: 1..`PLAYER_OUTPUTS`, required, 1 is Out 1; `min_size` 420×160). GET only (405 otherwise). It answers CORS only for an `Origin` that `WIDGET_FRAME_ANCESTORS` lists, with `Vary: Origin`, and not the API's `Access-Control-Allow-Origin: *`.
- `GET /widget/transport?output=<n>[&theme=dark|light|transparent]` is one output's transport without the app around it, on the player's own API: label, format, state, what it plays (pattern and test audio, item, or playlist and item), Play/Pause/Stop, Restart, Step, Loop, and the position with remaining time (click to seek a video). A bad output or theme answers 400, an unknown widget 404.
- New setting `WIDGET_FRAME_ANCESTORS` (default `'self'`): the widget pages' `Content-Security-Policy: frame-ancestors`. They send no `X-Frame-Options`. `;`, `,` or control characters exit 78.
- The page posts `{type: "widget-ready"}` and `{type: "widget-size", w, h}` to the page that frames it.
- The web UI's asset URLs are absolute (`/assets/…`), so the same `index.html` loads under `/widget/`.
- A 405 answer carries the reason `Method Not Allowed` (it said `OK`).
- The Compose and Kubernetes examples use the `1.2.0` image.

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
