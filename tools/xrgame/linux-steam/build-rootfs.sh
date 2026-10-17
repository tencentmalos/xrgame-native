#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run in the dedicated Ubuntu ARM64 Apple Container, with /project read-only
# and private /output. This userspace is for the Steam desktop client ONLY.
# No Wine, FEX, DXVK, VKD3D, Turnip, gamescope, game data or Valve binary is added.
set -euo pipefail
[[ $(dpkg --print-architecture) == arm64 && -d /output ]] || exit 2
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    ca-certificates python3 curl dbus-x11 bash coreutils xz-utils unzip procps findutils xdg-user-dirs \
    libnss3 libnspr4 libx11-6 libxext6 libxcb1 libxrandr2 libxfixes3 libxi6 libxcursor1 \
    libxrender1 libxtst6 libxss1 libxcomposite1 libxdamage1 libasound2t64 libpulse0 \
    libgtk-3-0t64 libgtk2.0-0t64 libgbm1 libdrm2 libgl1 libegl1 libva2 libcurl4t64 \
    libudev1 libfontconfig1 libibus-1.0-5 libpipewire-0.3-0t64 fonts-dejavu-core \
    libopenal1 libsm6 libice6 libvdpau1 libnm0 libsdl2-2.0-0 libssh2-1t64 \
    xserver-xephyr x11-utils libgl1-mesa-dri libglx-mesa0 lsof fontconfig fonts-noto-cjk xfonts-base xfonts-wqy
# CEF/VGUI use Fontconfig; the legacy bootstrap popup uses X core fonts.
# Install both paths: Noto alone does not populate Xephyr's XLFD font catalogue.
fc-cache -f
mkdir -p /usr/local/lib/steampsp /usr/share/steampsp
cp /project/tools/xrgame/linux-steam/install-client.py /usr/local/lib/steampsp/
cp /project/tools/xrgame/linux-steam/launch-client.py /usr/local/lib/steampsp/
dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\t${source:Package}\t${source:Version}\n' \
    > /usr/share/steampsp/rootfs-packages.tsv
cp /usr/share/steampsp/rootfs-packages.tsv /output/linux-steam-rootfs-packages.tsv
# Keep /etc credentials from the disposable build container out. This is an
# unprivileged client runtime; Android's UID remains authoritative under PRoot.
tar -C / -czf /output/linux-steam-rootfs.tar.gz \
    --exclude=etc/shadow --exclude=etc/gshadow --exclude=etc/hostname --exclude=etc/hosts \
    --exclude=etc/machine-id --exclude=etc/resolv.conf --exclude=etc/ld.so.cache --exclude=etc/ssl/private \
    usr bin lib sbin etc
sha256sum /output/linux-steam-rootfs.tar.gz > /output/linux-steam-rootfs.sha256
