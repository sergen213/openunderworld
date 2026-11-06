#!/usr/bin/env bash
# Builds the AppImage with build-appimage.sh inside the Ubuntu 24.04 image of ./Containerfile, so it runs on any
# x86_64 distro with glibc >= 2.39 (floor printed as "== floor:"; docs/RELEASING.md).
# Usage: packaging/appimage/build-in-container.sh [OUT_DIR]   (default: build-appimage-noble/out)
# Env:   OU_APPIMAGE_BUILD_DIR (default build-appimage-noble; incremental, owned by you), OU_TOOLS_CACHE, OU_JOBS (8).
# Needs docker usable without sudo. The source is mounted read-only.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${OU_APPIMAGE_BUILD_DIR:-$SRC/build-appimage-noble}"
OUT="${1:-$BUILD/out}"
CACHE="${OU_TOOLS_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/openunderworld-appimage-tools}"
command -v docker >/dev/null || { echo "build-in-container.sh: docker not found (install docker, or run build-appimage.sh for a host-glibc build)" >&2; exit 1; }
mkdir -p "$BUILD" "$OUT" "$CACHE"
IMAGE=openunderworld-appimage:noble
docker build -q -t "$IMAGE" -f "$SRC/packaging/appimage/Containerfile" "$SRC/packaging/appimage" >/dev/null
# git runs on the host: a release worktree's .git points outside the mounted source.
DESC="$(git -C "$SRC" describe --tags --match 'v[0-9]*' --dirty --always 2>/dev/null || echo unknown)"
exec docker run --rm --memory=12g --user "$(id -u):$(id -g)" \
  -v "$SRC:/src:ro" -v "$(cd "$BUILD" && pwd):/build" -v "$(cd "$OUT" && pwd):/out" -v "$CACHE:/tools" \
  -e OU_APPIMAGE_BUILD_DIR=/build -e OU_TOOLS_CACHE=/tools -e OU_JOBS="${OU_JOBS:-8}" -e OU_GIT_DESCRIBE="$DESC" \
  "$IMAGE" /src/packaging/appimage/build-appimage.sh /out
