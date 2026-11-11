ARG SUNUEFI_BASE_IMAGE=public.ecr.aws/docker/library/debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
FROM ${SUNUEFI_BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive SUNUEFI_TOOLCHAIN_ROOT=/usr
RUN if [ "$(dpkg --print-architecture)" != arm64 ]; then \
        dpkg --add-architecture arm64; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
    build-essential git make clang lld llvm nasm acpica-tools autoconf automake libtool \
    binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    rustc cargo libstd-rust-dev:arm64 libyuv-dev libjpeg62-turbo-dev \
    flex bison bc libssl-dev uuid-dev zlib1g-dev libelf-dev \
    python3 python3-venv python3-pip python3-packaging gettext fonts-noto-cjk libfreetype6 \
    debootstrap debian-archive-keyring qemu-user-static \
    mtools dosfstools e2fsprogs cpio kmod device-tree-compiler \
    zstd rsync ccache alsa-utils m4 pkg-config curl ca-certificates openssh-client openssl \
    util-linux file squashfs-tools busybox-static libarchive-tools libglib2.0-bin systemd-ukify && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
# No source or personal workspace is embedded in the builder image.
# APT packages are authenticated and their actual versions are recorded below.
RUN dpkg-query -W > /usr/share/sunuefi-build-packages.tsv

CMD ["bash"]
