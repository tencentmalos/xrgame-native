ARG XRGAME_BASE_IMAGE=ubuntu:24.04
FROM ${XRGAME_BASE_IMAGE}

# Toolchain archives/source revisions are pinned in prepare-linux.sh. Record the
# resolved Ubuntu package versions as evidence; this is not a bit-reproducible OS.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    autoconf automake autopoint bison build-essential ca-certificates cmake curl \
    file flex gettext git glslang-tools intltool libtool libltdl-dev libfreetype-dev \
    ninja-build patch pkg-config python3 python3-venv rsync texinfo unzip xz-utils zstd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
