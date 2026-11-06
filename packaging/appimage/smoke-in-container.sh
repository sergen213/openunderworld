#!/usr/bin/env bash
# Smoke-tests an AppImage in a clean Ubuntu 24.04 container with only desktop runtime packages (Ubuntu's Mesa,
# Xvfb): --self-test, --version, and with GAME_DIR an import scan, one OpenGL ou-game l4_pool frame (llvmpipe under
# Xvfb) and, when the host has /dev/dri, one Vulkan ou-viewer frame on the host GPU with Ubuntu's Mesa driver
# (SDL offscreen video, headless surface), saved as game.ppm / viewer.ppm in SHOT_DIR.
# lavapipe is not used for Vulkan: VkRenderer::drawFrame crashes on it (Arch builds too).
# Usage: packaging/appimage/smoke-in-container.sh APPIMAGE [GAME_DIR [SHOT_DIR]]
set -euo pipefail
APP="$(realpath "$1")"
GAME="${2:-}"
SHOTS="$(mkdir -p "${3:-.}" && cd "${3:-.}" && pwd)"
MOUNTS=(-v "$APP:/app/OU.AppImage:ro" -v "$SHOTS:/shots")
[ -z "$GAME" ] || MOUNTS+=(-v "$(realpath "$GAME"):/game:ro")
GPU=()
if [ -e /dev/dri ]; then
  GPU=(--device /dev/dri -e HAVE_GPU=1)
  for g in render video; do getent group $g >/dev/null && GPU+=(--group-add "$(getent group $g | cut -d: -f3)"); done
fi
docker run --rm --memory=12g "${MOUNTS[@]}" "${GPU[@]}" -e HAVE_GAME="${GAME:+1}" -e UID_GID="$(id -u):$(id -g)" \
  ubuntu:24.04 bash -euc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq --no-install-recommends libgl1 libegl1 libgl1-mesa-dri libvulkan1 \
    mesa-vulkan-drivers libwayland-client0 libx11-6 libx11-xcb1 libxcb1 libfontconfig1 fonts-dejavu-core \
    libdbus-1-3 libasound2t64 libharfbuzz0b libfribidi0 xvfb xauth >/dev/null
  export APPIMAGE_EXTRACT_AND_RUN=1 OU_DISABLE_UPDATES=1 HOME=/tmp/home QT_QPA_PLATFORM=offscreen LANG=C.UTF-8 \
    MESA_GL_VERSION_OVERRIDE=4.6 MESA_GLSL_VERSION_OVERRIDE=460 # llvmpipe in 24.04 reports 4.5
  mkdir -p $HOME && cd /tmp
  ldd --version | head -1
  /app/OU.AppImage --self-test
  /app/OU.AppImage --version
  [ -n "$HAVE_GAME" ] || exit 0
  /app/OU.AppImage import scan /game --no-hash -o /tmp/manifest.json
  test -s /tmp/manifest.json
  xvfb-run -a -s "-screen 0 1280x720x24" /app/OU.AppImage game --game-dir /game --level "pc-w\\l4_pool.drm" \
    --renderer opengl --frames 30 --screenshot /shots/game.ppm --no-audio
  if [ -n "${HAVE_GPU:-}" ]; then
    SDL_VIDEO_DRIVER=offscreen VK_LOADER_DRIVERS_DISABLE="*lvp*" /app/OU.AppImage viewer --game-dir /game --level \
      --drm "pc-w\\l4_pool.drm" --renderer vulkan --frames 30 --screenshot /shots/viewer.ppm
  else echo "no /dev/dri: Vulkan frame skipped"; fi
  chown "$UID_GID" /shots/*.ppm
'
