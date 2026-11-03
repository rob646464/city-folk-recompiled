#!/bin/bash
# Linux half of the release (called by tools/release_windows.ps1 through WSL, or by hand):
#   tools/release_linux.sh <version> [out dir]
# Builds in a clone in the Linux filesystem (~/cf-release; building on /mnt/c is very slow),
# made from this checkout's committed HEAD, installs missing build packages (apt, asks for the
# sudo password once), finds or downloads the official LLVM release whose clang/ld.lld are
# bundled, runs tools/package_linux.sh and copies
#   CityFolkRecompiled-<version>-linux-x86_64.tar.gz / .zip
# into <out dir> (default: out/ of this checkout). No game data is involved.
set -euo pipefail
VERSION=${1:?usage: release_linux.sh <version> [out dir]}
SRC=$(cd "$(dirname "$0")/.." && pwd)
DEST=$(mkdir -p "${2:-$SRC/out}" && cd "${2:-$SRC/out}" && pwd)
WORK=${CF_RELEASE_WORK:-$HOME/cf-release}
LLVM_FALLBACK=20.1.8   # downloaded only when no LLVM-*-Linux-X64 is found below

step() { printf '\n==> %s\n' "$*"; }

step "Build packages"
PKGS="clang lld llvm cmake ninja-build pkg-config git python3 zip patchelf xz-utils curl
libvulkan-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev
libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols
libdecor-0-dev libegl-dev libgl-dev libgles-dev libasound2-dev libpulse-dev
libpipewire-0.3-dev libdbus-1-dev libudev-dev libibus-1.0-dev libdrm-dev libgbm-dev libusb-1.0-0-dev"
MISSING=$(for p in $PKGS; do dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'ok installed' || echo "$p"; done)
if [ -n "$MISSING" ]; then
    echo "Installing:" $MISSING
    sudo apt-get update
    # shellcheck disable=SC2086
    sudo apt-get install -y $MISSING
else
    echo "all installed"
fi

step "Source ($WORK, from $SRC)"
# The Windows checkout under /mnt/c belongs to another user as far as git is concerned.
git() { command git -c safe.directory='*' "$@"; }
# A git worktree's .git is a file naming its git dir, as a Windows path (C:/...) for a checkout
# made on Windows: Linux git cannot follow it, so read it and fetch from the git dirs directly.
gitdir_of() {  # <checkout> -> its git dir (a Linux path)
    if [ -f "$1/.git" ]; then
        local d
        d=$(sed -n 's/^gitdir: //p' "$1/.git" | tr -d '')
        case "$d" in
            [A-Za-z]:/*) d="/mnt/$(printf %s "${d%%:*}" | tr 'A-Z' 'a-z')/${d#?:/}" ;;
            /*) ;;
            *) d="$1/$d" ;;
        esac
        echo "$d"
    else
        echo "$1/.git"
    fi
}
SRC_GIT=$(gitdir_of "$SRC")
SHA=$(git --git-dir="$SRC_GIT" rev-parse HEAD)
if [ ! -d "$WORK/.git" ]; then
    git init -q "$WORK"
fi
git -C "$WORK" fetch --force "$SRC_GIT" "$SHA" || { echo "cannot fetch $SHA from $SRC_GIT" >&2; exit 1; }
git -C "$WORK" checkout --force --detach FETCH_HEAD
git -C "$WORK" clean -fdq
# Aurora from this checkout's submodule (has the pinned commit; no network needed).
AURORA_GIT=$(gitdir_of "$SRC/extern/aurora")
git -C "$WORK" config submodule.extern/aurora.url "$AURORA_GIT"
git -C "$WORK" -c protocol.file.allow=always submodule update --init --recursive --force
# --force reset Aurora to pristine: drop the applied-patches stamp so configure applies them
# again (with an unchanged patch set it would otherwise skip them and the build fails).
rm -f "$(git -C "$WORK/extern/aurora" rev-parse --absolute-git-dir)/cf_patches_applied"
git -C "$WORK" log --oneline -1

step "LLVM to bundle"
LLVM_BIN=${CF_LINUX_LLVM:-}
if [ -z "$LLVM_BIN" ]; then
    LLVM_BIN=$( (find "$HOME" /opt -maxdepth 6 -path '*LLVM-*-Linux-X64/bin/ld.lld' 2>/dev/null || true) | sort -V | tail -1 | xargs -r dirname)
fi
if [ -z "$LLVM_BIN" ]; then
    NAME=LLVM-$LLVM_FALLBACK-Linux-X64
    echo "No LLVM release found; downloading $NAME (about 1 GB)"
    mkdir -p "$HOME/llvm"
    curl -fL --retry 3 -o "$HOME/llvm/$NAME.tar.xz" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_FALLBACK/$NAME.tar.xz"
    tar -xf "$HOME/llvm/$NAME.tar.xz" -C "$HOME/llvm"
    rm -f "$HOME/llvm/$NAME.tar.xz"
    LLVM_BIN=$HOME/llvm/$NAME/bin
fi
echo "$LLVM_BIN"

step "Build and package (this can take a while)"
"$WORK/tools/package_linux.sh" "$LLVM_BIN" "$WORK/out"

step "Copy to $DEST"
for ext in tar.gz zip; do
    cp -f "$WORK/out/CityFolkRecompiled-linux-x86_64.$ext" "$DEST/CityFolkRecompiled-$VERSION-linux-x86_64.$ext"
    ls -la "$DEST/CityFolkRecompiled-$VERSION-linux-x86_64.$ext"
done
