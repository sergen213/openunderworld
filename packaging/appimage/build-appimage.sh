#!/usr/bin/env bash
# Builds OpenUnderworld-<version>-x86_64.AppImage (+ .sha256, + .zsync when zsyncmake exists).
# Usage: packaging/appimage/build-appimage.sh [OUT_DIR]     (default: build-appimage/out)
# Env:   OU_APPIMAGE_BUILD_DIR (default build-appimage), OU_TOOLS_CACHE (default ~/.cache/openunderworld-appimage-tools),
#        OU_JOBS (default 12). No game data goes in: only OpenUnderworld's own binaries. Notes: docs/RELEASING.md.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${OU_APPIMAGE_BUILD_DIR:-$SRC/build-appimage}"
OUT="$(mkdir -p "${1:-$BUILD/out}" && cd "${1:-$BUILD/out}" && pwd)"
CACHE="${OU_TOOLS_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/openunderworld-appimage-tools}"
APPDIR="$BUILD/AppDir"

# ---- version: CMake project(VERSION) + git describe ------------------------------------------------
VERSION="$(sed -nE 's/^project\(OpenUnderworld VERSION ([0-9.]+).*/\1/p' "$SRC/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "no project(VERSION) in CMakeLists.txt" >&2; exit 1; }
DESC="$(git -C "$SRC" describe --tags --match 'v[0-9]*' --dirty --always 2>/dev/null || echo unknown)"
case "$DESC" in
  "v$VERSION") FULL="$VERSION" ;;                    # exactly on the release tag
  "v$VERSION"-*) FULL="${DESC#v}" ;;                 # 0.1.0-3-gabc1234[-dirty]
  *) FULL="$VERSION-git.$DESC" ;;                    # no matching tag yet
esac
NAME="OpenUnderworld-$FULL-x86_64.AppImage"
echo "== OpenUnderworld $FULL"

# ---- pinned packaging tools, verified by SHA-256 (digests as published on the GitHub releases) -----
fetch() { # url sha256 file
  local f="$CACHE/$3"
  if [ ! -f "$f" ] || ! echo "$2  $f" | sha256sum -c --quiet - 2>/dev/null; then
    mkdir -p "$CACHE"
    curl -fsSL --proto '=https' --tlsv1.2 -o "$f.part" "$1"
    echo "$2  $f.part" | sha256sum -c --quiet - || { rm -f "$f.part"; echo "checksum mismatch: $1" >&2; exit 1; }
    mv "$f.part" "$f"
  fi
  chmod +x "$f"
  echo "$f"
}
LINUXDEPLOY="$(fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
  c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d linuxdeploy-1-alpha-20251107-1-x86_64.AppImage)"
APPIMAGETOOL="$(fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
  ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-1.9.1-x86_64.AppImage)"
RUNTIME="$(fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
  2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d type2-runtime-20251108-x86_64)"
# linuxdeploy's bundled patchelf 0.15 corrupts current Arch libraries (libavformat, libgnutls, libzmq then
# segfault in ld.so); linuxdeploy honours $PATCHELF, so a pinned 0.19.2 is used instead.
PATCHELF_TGZ="$(fetch https://github.com/NixOS/patchelf/releases/download/0.19.2/patchelf-0.19.2-x86_64.tar.gz \
  2abdc34fbe949c995a1baa4ec310896a085588a45a99cd0d726851bdfac2a4bb patchelf-0.19.2-x86_64.tar.gz)"
[ -x "$CACHE/patchelf-0.19.2" ] || tar -xzOf "$PATCHELF_TGZ" ./bin/patchelf > "$CACHE/patchelf-0.19.2"
chmod +x "$CACHE/patchelf-0.19.2"
export PATCHELF="$CACHE/patchelf-0.19.2"
export APPIMAGE_EXTRACT_AND_RUN=1 # the tools themselves need no FUSE (containers, CI)

# ---- Release build (no sanitizers, no tests) -------------------------------------------------------
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DOU_SANITIZE=OFF -DOU_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$BUILD" -j "${OU_JOBS:-12}"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD" >/dev/null
for t in ou-launcher ou-game ou-viewer ou-movie ou-audio ou-import ou-asset ou-text ou-sign; do
  [ -x "$APPDIR/usr/bin/$t" ] || { echo "missing $t in AppDir" >&2; exit 1; }
done

# ---- Qt: manual bundling (linuxdeploy-plugin-qt is not installed here; see docs/RELEASING.md) -------
# Only ou-launcher uses Qt (Widgets). Copy the platform plugins it needs, then let linuxdeploy pull
# their library dependencies; qt.conf next to the binaries points Qt at usr/plugins.
QT_PLUGINS="$(qmake6 -query QT_INSTALL_PLUGINS)"
for p in platforms/libqxcb.so platforms/libqwayland.so platforms/libqoffscreen.so \
         wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so \
         wayland-decoration-client/libadwaita.so platformthemes/libqxdgdesktopportal.so \
         imageformats/libqsvg.so imageformats/libqico.so imageformats/libqjpeg.so iconengines/libqsvgicon.so; do
  [ -f "$QT_PLUGINS/$p" ] || { echo "note: Qt plugin $p not found, skipped"; continue; }
  install -Dm755 "$QT_PLUGINS/$p" "$APPDIR/usr/lib/qt6/plugins/$p"
done
printf '[Paths]\nPrefix = ..\nPlugins = lib/qt6/plugins\n' > "$APPDIR/usr/bin/qt.conf"

# ---- dependencies: GPU driver stack and the C/C++ runtime stay on the host -------------------------
# linuxdeploy's built-in excludelist already drops glibc, libGL/libEGL/libGLX, libvulkan, libdrm, X11/xcb
# and friends; the explicit list below makes the policy visible and covers newer names.
# libstdc++/libgcc_s stay on the host too: Mesa's drivers link the host libstdc++, an older bundled copy
# would break them. Consequence: the host needs a libstdc++/glibc at least as new as the build host's.
EXCLUDES=()
for x in 'libGL.so*' 'libGLX*' 'libEGL*' 'libGLES*' 'libOpenGL*' 'libGLdispatch*' 'libvulkan*' 'libdrm*' \
         'libgbm*' 'libOpenCL*' 'libva*' 'libvdpau*' 'libnvidia*' 'libwayland-client*' 'libwayland-egl*' 'libwayland-cursor*' \
         'libstdc++*' 'libgcc_s*' 'libasound*' 'libpulse*' 'libpipewire*' 'libdbus-1*' \
         'libfontconfig*' 'libfreetype*' 'libudev*' 'libsystemd*'; do
  EXCLUDES+=(--exclude-library "$x")
done
# linuxdeploy's bundled strip predates .relr.dyn (current Arch libraries): host libs are stripped
# already, so only our own binaries are stripped, with the host strip.
strip --strip-unneeded "$APPDIR"/usr/bin/ou-*
NO_STRIP=1 "$LINUXDEPLOY" --appdir "$APPDIR" "${EXCLUDES[@]}" \
  --deploy-deps-only "$APPDIR/usr/bin" --deploy-deps-only "$APPDIR/usr/lib/qt6/plugins" \
  --desktop-file "$APPDIR/usr/share/applications/openunderworld.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/openunderworld.svg" \
  --custom-apprun "$SRC/packaging/appimage/AppRun" --verbosity 2

bad="$(find "$APPDIR/usr/lib" -maxdepth 1 \( -name 'libGL*' -o -name 'libEGL*' -o -name 'libvulkan*' -o -name 'libdrm*' \
        -o -name 'libgbm*' -o -name 'libOpenCL*' -o -name 'libstdc++*' -o -name 'libc.so*' \) -printf '%f ' 2>/dev/null || true)"
[ -z "$bad" ] || { echo "host-only libraries leaked into the AppDir: $bad" >&2; exit 1; }
# Every tool must get past the dynamic loader with the bundled libraries (catches broken patchelf output).
for t in "$APPDIR"/usr/bin/ou-*; do
  rc=0; QT_QPA_PLATFORM=offscreen "$t" --help >/dev/null 2>&1 || rc=$?
  [ "$rc" -lt 128 ] || { echo "$t --help died with signal $((rc - 128))" >&2; exit 1; }
done
# Every bundled ELF outside usr/lib must find usr/lib through its RUNPATH (no reliance on LD_LIBRARY_PATH).
while IFS= read -r f; do
  rp="$(readelf -d "$f" | sed -nE 's/.*\((RUNPATH|RPATH)\).*\[(.*)\]/\2/p')"
  ok=0
  IFS=: read -ra parts <<< "$rp"
  for p in "${parts[@]}"; do
    [ "$(realpath -m "${p//\$ORIGIN/$(dirname "$f")}")" = "$(realpath "$APPDIR/usr/lib")" ] && ok=1
  done
  [ "$ok" = 1 ] || { echo "RUNPATH of $f does not reach usr/lib: '$rp'" >&2; exit 1; }
done < <(find "$APPDIR/usr/bin" "$APPDIR/usr/lib/qt6/plugins" -type f \( -name 'ou-*' -o -name '*.so' \))

# ---- pack -------------------------------------------------------------------------------------------
rm -f "$OUT/$NAME" "$OUT/$NAME.sha256" "$OUT/$NAME.zsync"
UPDINFO="gh-releases-zsync|sergen213|openunderworld|latest|OpenUnderworld-*-x86_64.AppImage.zsync"
( cd "$OUT" && ARCH=x86_64 VERSION="$FULL" "$APPIMAGETOOL" --no-appstream --runtime-file "$RUNTIME" \
    -u "$UPDINFO" "$APPDIR" "$NAME" >/dev/null )
( cd "$OUT" && sha256sum "$NAME" > "$NAME.sha256" )
echo "== $OUT/$NAME ($(du -h "$OUT/$NAME" | cut -f1))"
ls "$OUT/$NAME".* 2>/dev/null
