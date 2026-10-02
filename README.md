# mxl-test-player

TAI-locked MXL test source. Each output writes a `video/v210` flow and an `audio/float32` flow (optional `video/smpte291` timecode and a fill/key pair) and publishes them as an NMOS sender group.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# web UI (no CDN; Vue is bundled)
cd web && npm install && npm run build && cd ..
./build/mxl-test-player
```

Open `http://localhost:8130`. NMOS node API defaults to port 3282.

Configuration is environment over `/config/player.json` over the defaults in the spec. Invalid configuration exits 78. A failure to open the MXL domain exits 75. SIGTERM exits 143.

Library files live in `PLAYER_LIBRARY_DIR` (default `/data/library`). The MXL domain is `MXL_OUTPUT_DOMAIN_DIR` (default `/Volumes/mxl/player-<seed>`).

```bash
./build/player_tests
python3 tests/integration/play_loop.py
```

See `IMPLEMENTATION_PLAN.md` for SDK pin notes and deviations, and `deploy/` for Compose, Kubernetes, and Grafana.
