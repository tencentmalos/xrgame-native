ARG XRGAME_BASE_IMAGE=ubuntu:24.04
FROM ${XRGAME_BASE_IMAGE}

# Toolchain archives/source revisions are pinned in prepare-linux.sh. Record the
# resolved Ubuntu package versions as evidence; this is not a bit-reproducible OS.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    autoconf automake autopoint bison build-essential ca-certificates cmake curl \
    file flex gawk gettext git glslang-tools intltool libtool libltdl-dev libfreetype-dev \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu x11proto-dev \
    ninja-build openjdk-17-jdk-headless patch pkg-config python3 python3-venv rsync texinfo unzip xz-utils zip zstd \
    && rm -rf /var/lib/apt/lists/*

ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
WORKDIR /opt/steampsp/project
