# Use the Rust channel selected by Denial's signed release.
FROM docker.io/library/rust:1.98.0-slim-trixie@sha256:17d1ba895198f9934c6314ec5346a0d5115372f3243390c3d731e242f35c2f27

RUN apt-get -o APT::Update::Error-Mode=any update \
    && apt-get install -y --no-install-recommends \
       build-essential ca-certificates git gpg pkg-config python3 \
       libdrm-dev libegl-dev libfontconfig-dev libgbm-dev libinput-dev \
       libseat-dev libudev-dev libxkbcommon-dev \
    && rm -rf /var/lib/apt/lists/*

ENV LC_ALL=C CARGO_NET_GIT_FETCH_WITH_CLI=true
WORKDIR /workspace
