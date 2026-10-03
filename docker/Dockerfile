# syntax=docker/dockerfile:1
# Image layout matches the sibling MXL media functions:
# web UI, then a build stage that pins MXL, then a runtime image.
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ubuntu:24.04 AS build
ARG MXL_REF
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config \
        libfreetype6-dev libavcodec-dev libavformat-dev libavutil-dev \
        libswscale-dev libswresample-dev nlohmann-json3-dev uuid-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
COPY --from=webui /web/dist /src/web/dist
WORKDIR /src
# FetchContent pulls MXL at the pin recorded in CMakeLists.txt (MXL_REF is the
# image label; the CMake pin is the build pin and must stay the same commit).
RUN cmake -S /src -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
    && cmake --build /tmp/build -j"$(nproc)" \
    && /tmp/build/player_tests

FROM ubuntu:24.04 AS runtime
ARG MXL_REF
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates ffmpeg libfreetype6 \
    && rm -rf /var/lib/apt/lists/* \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin mxl; \
       fi

COPY --from=build /tmp/build/mxl-test-player /usr/local/bin/mxl-test-player
COPY --from=build /src/web/dist /usr/local/share/mxl-test-player/web
COPY --from=build /src/assets/fonts /usr/local/share/mxl-test-player/fonts

ENV PLAYER_WEB_ROOT=/usr/local/share/mxl-test-player/web \
    PLAYER_FONT_DIR=/usr/local/share/mxl-test-player/fonts
LABEL io.dmf.mxl.revision="${MXL_REF}"
LABEL org.opencontainers.image.title="mxl-test-player"
LABEL org.opencontainers.image.description="MXL test and demo source"
LABEL org.opencontainers.image.licenses="MIT"
USER 1000:1000
EXPOSE 8130/tcp 3282/tcp
ENTRYPOINT ["/usr/local/bin/mxl-test-player"]
