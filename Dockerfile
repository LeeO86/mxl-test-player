# Dev image uses distro FFmpeg (6.1 on Ubuntu 24.04). Spec pins 7.x; see IMPLEMENTATION_PLAN.md.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config curl \
        libfreetype6-dev libavcodec-dev libavformat-dev libavutil-dev \
        libswscale-dev libswresample-dev nlohmann-json3-dev uuid-dev \
        ffmpeg fonts-dejavu-core \
        nodejs npm \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cd web && npm install && npm run build \
    && cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/build -j --target mxl-test-player \
    && cmake --install /tmp/build --prefix /usr/local \
    && cp -a web/dist /usr/local/share/mxl-test-player/web \
    && cp -a assets/fonts /usr/local/share/mxl-test-player/fonts

USER 1000
EXPOSE 8130 3282
ENV PLAYER_WEB_ROOT=/usr/local/share/mxl-test-player/web \
    PLAYER_FONT_DIR=/usr/local/share/mxl-test-player/fonts
LABEL io.dmf.mxl.revision="0.3.0"
ENTRYPOINT ["/usr/local/bin/mxl-test-player"]
