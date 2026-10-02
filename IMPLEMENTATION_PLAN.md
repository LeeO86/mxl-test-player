# Implementation plan

Status: v1 implemented against draft spec v0.3. Deviations are recorded here, as the spec requires.

## Decisions

- **Image decode** uses FFmpeg/libav (PNG, JPEG, TIFF, WebP, GIF, APNG). No separate image library.
- **Mezzanine** is ProRes 422 HQ (`prores_ks` profile 3) in MOV with PCM float32 audio. Measured on this host: a 0.4 s 640×360 source converts to 720p25 ProRes and plays from RAM with zero underruns (see `tests/integration/play_loop.py`).
- **Text** is FreeType with bundled DejaVu Sans, DejaVu Sans Bold, and DejaVu Sans Mono.
- **Interlaced MXL grains are fields.** Pinned MXL `218ddaa` (`FlowParser`) requires `grain_rate` 25/1 or 30000/1001 in the flow JSON for `interlaced_tff`/`interlaced_bff`, then doubles it so each grain is one field. The spec's parenthetical "grain = frame" does not match that SDK. Picture burn-in timecode stays at the frame rate; the ANC field flag marks the field. 1080i50 in the integration wording is treated as 1080i25 (50 fields).
- **59.94 audio cadence is 800/801 samples**, not 1601/1602. 1601/1602 is the 29.97 frame cadence. Both are tested. The count is the truncating sum `samples(n+1) - samples(n)` with `samples(n) = n * 48000 * den / num`, which is exact at the 1001-frame cycle.
- **v210a** is implemented as the pinned SDK lays it out: v210 fill plane followed by a 10-bit alpha plane (`(width+2)/3*4` bytes per line). `mxl-multiviewer` and Strom were not in this environment, so receiver support is unverified. `fill_key` (two `video/v210` flows) is the default keyed mode. The v210 key uses legal luma 64–940; the v210a alpha plane uses full-range 0–1023.
- **NMOS** is an in-process IS-04 v1.3 node plus IS-05 v1.1 single-sender connection API with BCP-007-03 MXL transport (`urn:x-nmos:transport:mxl`, empty `interface_bindings`, null manifest, `mxl_domain_id` / `mxl_flow_id`). `nmos-cpp` is not linked. DNS-SD defaults off and is not announced when `NMOS_DNS_SD=true` (the flag is accepted and logged by remaining unused). Registry registration is best-effort HTTP to `NMOS_REGISTRY_ADDRESS`.
- **FFmpeg** on the image used to develop this tree is 6.1 (libavcodec 60). The code uses the 6.x decode API (`AVChannelLayout`, `AVFrame::duration`). A 7.x runtime is the spec pin; the Dockerfile builds against the distro FFmpeg and records that gap.
- **Conversion** runs `ffmpeg` for the video mezzanine (bwdif for interlaced sources, `fps` or `minterpolate`, `tinterlace` when the target is interlaced, `colorspace` when the source is tagged BT.601) and libav for stills and sprites. Audio is resampled, optionally `loudnorm`'d to −23 LUFS, then conformed in-process so the sample count equals the frame cadence. Channel mapping keeps source channels and pads with silence.
- **RAM clips** are used when duration ≤ `PLAYER_RAM_CLIP_MAX_S` and the v210 payload fits `PLAYER_RAM_BUDGET_MB`. Longer files are not decode-ahead queued in this revision: playback of those items repeats black and counts underruns until a RAM-sized conversion is available. Patterns, stills, and RAM clips are written on the TAI clock with a repeat-last underrun path.
- **Moving-box continuity.** Position is a pure function of the TAI grain index. A parameter edit stores a pixel delta so the box does not jump; that delta is part of output state, so a restart reproduces it. With a zero delta the position matches across restarts.
- **Performance targets** (4×1080p50 for 1 h, 2×2160p50, 16× RAM) were not run on a Precision 3930 or R740. A single 720p25 pattern output on this container wrote grains with zero underruns over the integration window.

## Build

MXL `release/v1.1` commit `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` is fetched at configure time and compiled with fabrics off. fmt, spdlog, stduuid, and picojson are fetched the same way. The player links that static `libmxl`.
