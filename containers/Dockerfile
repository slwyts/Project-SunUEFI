FROM debian@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a

ENV DEBIAN_FRONTEND=noninteractive SUNUEFI_TOOLCHAIN_ROOT=/usr
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git make clang lld llvm nasm acpica-tools \
    binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu \
    flex bison bc libssl-dev uuid-dev zlib1g-dev libelf-dev \
    python3 python3-venv python3-pip python3-packaging gettext fonts-noto-cjk \
    debootstrap debian-archive-keyring qemu-user-static \
    mtools dosfstools e2fsprogs cpio kmod device-tree-compiler \
    zstd rsync ccache alsa-utils m4 pkg-config curl ca-certificates \
    util-linux file && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
# No source or personal workspace is embedded in the builder image.
# APT packages are authenticated and their actual versions are recorded below.
RUN dpkg-query -W > /usr/share/sunuefi-build-packages.tsv

CMD ["bash"]
