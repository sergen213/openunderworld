# Releasing

Status labels: [O] observed (run and seen), [I] inferred from the code, [U] unknown.

Official releases are **local and signed**: `tools/release.sh` on the maintainer workstation, which holds the Ed25519 key in `~/.config/openunderworld-signing/release.key`. CI builds unsigned test AppImages only; it never sees the key.

## AppImage in an old-glibc container: `packaging/appimage/build-in-container.sh [OUT_DIR]`

Release and CI AppImages are built inside `packaging/appimage/Containerfile`: Ubuntu 24.04 (glibc 2.39, `g++-14` for C++23). The AppImage then runs on any x86_64 distribution at least as new as the measured floor below. You need only Docker usable without sudo (`docker` group); everything else is in the image:

- from Ubuntu 24.04: GCC 14 (`g++-14`, `CC`/`CXX` set in the image; `build-appimage.sh` passes `$CXX` by full path), CMake 3.28, Ninja, Python 3, libepoxy, zlib, OpenSSL 3, nlohmann-json, libsodium, libcurl, libshaderc + `glslc`, the Vulkan loader, ffmpeg 6.1 (`libav*`), the X11/Wayland/PipeWire/PulseAudio/ALSA headers SDL needs;
- pinned downloads checked by SHA-256 (too old or missing in 24.04): SDL `3.4.18` (release tarball), Vulkan-Headers `v1.4.363` (24.04 has 1.3.275, without `VK_AMD_anti_lag`), glm `1.0.3`, and Qt `6.8.3` (24.04 has 6.4; `find_package(Qt6 6.5)`): `qtbase`, `qtwayland`, `qtsvg` and ICU from the official online-installer repository (`qt6_683/…linux_gcc_64`, built on RHEL 8.10). The pinned SHA-256s are of archives whose SHA-1s match the `.sha1` files Qt publishes next to them.

`docker build` of the image takes about a minute and is cached. The wrapper then runs `build-appimage.sh` (below) with `--user $(id -u):$(id -g)`, the source mounted read-only at `/src`, the build directory (`build-appimage-noble/`, or `$OU_APPIMAGE_BUILD_DIR`) at `/build`, so rebuilds are incremental, and `OUT_DIR` at `/out`; the files belong to you. `git describe` runs on the host (`OU_GIT_DESCRIBE`), because a release worktree's `.git` points outside the mount. Memory is capped at 12 GiB, jobs at `OU_JOBS` (8).

Changes the container needed in `build-appimage.sh`: Qt 6.8 names its Wayland platform plugins `libqwayland-generic`/`libqwayland-egl` (+ `wayland-graphics-integration-client/libqt-plugin-wayland-egl`); the official Qt binaries carry RUNPATH `$ORIGIN/../../lib`, so the copied plugins get `$ORIGIN/../../..` (`usr/lib`); and the OpenCL / VA-API / VDPAU loaders are now bundled, because Ubuntu's ffmpeg links them and desktops do not always have them (their drivers would still come from the host; we never use hardware decoding).

**Floor** (printed by `build-appimage.sh` as `== floor:`; the newest symbol versions any `ou-*` or bundled library in the AppDir needs, `objdump -T`): **`GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.15` (GCC 14: `__cxa_call_terminate`)** [O] (2026-11-06, 0.31.0 + C++23, `g++-14` 14.2.0). glibc 2.38 comes from the bundled Ubuntu libraries themselves (librsvg, libxml2, libx264, …); 24.04's own `libstdc++6` is GCC 14's, so the floor stays within 24.04. In practice: Ubuntu 24.04, Debian 13, Fedora 40 or newer, current Arch/openSUSE Tumbleweed (Fedora 39's GCC 13 libstdc++ lacks `CXXABI_1.3.15`). `tools/release.sh` writes the measured numbers into the release notes' *Requirements* line.

What the host must provide (never bundled): glibc and libstdc++ at the floor; the GPU stack (`libGL`/`libEGL`/`libvulkan`/`libdrm`/`libgbm` and a Mesa or vendor driver for OpenGL 4.6 / Vulkan 1.3); X11/xcb or Wayland client libraries; fontconfig, freetype, harfbuzz, fribidi, D-Bus; ALSA/PulseAudio/PipeWire for sound; FUSE 2 (`libfuse2`) or `--appimage-extract-and-run`. On Ubuntu 24.04 that is `libgl1 libegl1 libgl1-mesa-dri libvulkan1 mesa-vulkan-drivers libwayland-client0 libx11-6 libx11-xcb1 libxcb1 libfontconfig1 libdbus-1-3 libasound2t64 libharfbuzz0b libfribidi0` (all on a desktop install).

**Clean-container smoke:** `packaging/appimage/smoke-in-container.sh APPIMAGE [GAME_DIR [SHOT_DIR]]` installs exactly that list (+ Xvfb) in a fresh `ubuntu:24.04` and runs `--self-test` and `--version` (Qt offscreen, extract-and-run), then with a game directory (mounted read-only) `import scan --no-hash`, an `ou-game` OpenGL frame of `l4_pool` on llvmpipe under Xvfb (`MESA_GL_VERSION_OVERRIDE=4.6`: 24.04's llvmpipe reports 4.5), an `ou-game` Vulkan frame on lavapipe (`SDL_VIDEO_DRIVER=offscreen`), and, when the host has `/dev/dri`, an `ou-viewer` Vulkan frame on the host GPU through Ubuntu's Mesa (lavapipe disabled).

### Old-glibc verification (2026-11-06, 0.30.0 + this change)

- [O] Built with `OU_APPIMAGE_BUILD_DIR=… packaging/appimage/build-in-container.sh OUT`: 94 MB, 155 bundled libraries; floor as above. An incremental rebuild (packaging change only) took 24 s.
- [O] `smoke-in-container.sh` in a clean `ubuntu:24.04` (glibc 2.39, Mesa 25.2.8): `--self-test` exit 0; `--version` → `ou-launcher 0.30.0`; `import scan` of the GOG install → `exe: GOG 1900745791`, 3218 hashes; `ou-game` GL frame on llvmpipe (Lara on the l4_pool stairs, screenshot viewed); `ou-viewer` Vulkan frame on RADV (RX 7900 XTX, Mesa 25.2.8), same framing as the Arch host's frame of the same AppImage.
- [O] The same AppImage on the Arch host (glibc 2.44, Mesa 26.2): `--self-test` exit 0, `viewer --renderer vulkan` on RADV writes its screenshot.
- [O] Vulkan on lavapipe crashed (a `memcpy` into a null mapping), also with the Arch-built v0.30.0 AppImage in an `archlinux` container: lavapipe's single memory type is device-local and host-visible, and a mapped allocation landed in an unmapped device-local block. Fixed after 0.30.0 (`LinearAllocator` chains mapped and unmapped blocks apart; FSR's memory-type pick falls back on UMA, ffx ou patch 8); ctest `vulkan_lavapipe` covers it. RADV cannot present to Xvfb (no DRI3), hence SDL's offscreen driver.
- [O] C++23 / `g++-14` (2026-11-06, 0.31.0-8): container build from scratch 7 min (`OU_JOBS=6`, LTO), 93 MB, floor as above. `smoke-in-container.sh` in a clean `ubuntu:24.04` (glibc 2.39, Mesa 25.2.8): `--self-test`, `--version` → `ou-launcher 0.31.0`, `import scan` → `exe: GOG 1900745791`, `ou-game` GL frame on llvmpipe, `ou-game` Vulkan frame on lavapipe, `ou-viewer` Vulkan frame on RADV. The lavapipe step had failed (`ou-game` exit 77, "doesn't implement VK_KHR_surface"; the released 0.31.0 AppImage too): Mesa 25 in noble-updates names the ICD `lvp_icd.json`, not `lvp_icd.x86_64.json`; the script now globs both.

## AppImage: `packaging/appimage/build-appimage.sh [OUT_DIR]`

Run directly, it builds against the host's libraries (on Arch: glibc 2.44, so the AppImage only runs on equally new hosts). Releases and CI run it through `build-in-container.sh` above.

1. **Version.** `project(VERSION)` from `CMakeLists.txt` plus `git describe --tags --match 'v*' --dirty`:
   - exactly on tag `vX.Y.Z` → `X.Y.Z`;
   - after it → `X.Y.Z-N-gHASH[-dirty]`;
   - no tag yet → `X.Y.Z-git.HASH[-dirty]`.
   Output: `OpenUnderworld-<version>-x86_64.AppImage`, `.sha256`, and `.zsync` (when `zsyncmake` exists; update information `gh-releases-zsync|sergen213|openunderworld|latest|OpenUnderworld-*-x86_64.AppImage.zsync`).
2. **Pinned tools**, downloaded once into `~/.cache/openunderworld-appimage-tools` and checked against SHA-256 digests (those GitHub publishes for the release assets); a mismatch aborts:

   | Tool | Release | Why |
   |---|---|---|
   | linuxdeploy | `1-alpha-20251107-1` | dependency deployment, RUNPATHs, desktop/icon |
   | appimagetool | `1.9.1` | squashfs + runtime |
   | type2-runtime | `20251108` | AppImage runtime, passed with `--runtime-file` (appimagetool would otherwise fetch it unverified) |
   | patchelf | `0.19.2` | linuxdeploy's bundled patchelf 0.15 corrupts current Arch libraries: `libavformat`, `libgnutls` and `libzmq` then segfault inside `ld.so` [O]. linuxdeploy uses `$PATCHELF`. |

   The tools run with `APPIMAGE_EXTRACT_AND_RUN=1`, so no FUSE is needed (containers).
3. **Release build** into `build-appimage/` (or `$OU_APPIMAGE_BUILD_DIR`): `-DCMAKE_BUILD_TYPE=Release -DOU_SANITIZE=OFF -DOU_BUILD_TESTS=OFF`, then `cmake --install` into the AppDir. All nine tools land in `usr/bin`: `ou-launcher` (entry point), `ou-game`, `ou-viewer`, `ou-movie`, `ou-audio`, `ou-import`, `ou-asset`, `ou-text`, `ou-sign`. The desktop file and the icon come from `packaging/`.
4. **Qt, bundled by hand.** `linuxdeploy-plugin-qt` is not used, and no unverified tool is downloaded. Only `ou-launcher` uses Qt (Widgets), so the script:
   - copies the plugins it needs from `qmake6 -query QT_INSTALL_PLUGINS` to `usr/lib/qt6/plugins/` (missing names are skipped; Qt 6.8 vs 6.10+ naming): `platforms/{libqxcb,libqwayland,libqwayland-generic,libqwayland-egl,libqoffscreen}`, `wayland-graphics-integration-client/libqt-plugin-wayland-egl`, `wayland-shell-integration/libxdg-shell`, `wayland-decoration-client/{libbradient,libadwaita}`, `platformthemes/libqxdgdesktopportal`, `imageformats/{libqsvg,libqico,libqjpeg}`, `iconengines/libqsvgicon`, and sets their RUNPATH to `$ORIGIN/../../..`;
   - lets linuxdeploy deploy their dependencies (`--deploy-deps-only`);
   - writes `usr/bin/qt.conf` (`Prefix = ..`, `Plugins = lib/qt6/plugins`).

   The plugins sit under `usr/lib/qt6/plugins` because linuxdeploy writes a fixed `$ORIGIN/../../../` RUNPATH for them; under `usr/plugins` that resolved to the AppDir root [O].
   No KDE/GTK platform theme is bundled (they would pull in KF6/GTK); the launcher uses Qt's Fusion style.
5. **Host-only libraries.** linuxdeploy's built-in excludelist plus an explicit list keep these out:
   - the GPU stack: `libGL*`, `libGLX*`, `libEGL*`, `libGLES*`, `libOpenGL*`, `libGLdispatch*`, `libvulkan*`, `libdrm*`, `libgbm*`, `libnvidia*`, `libwayland-{client,egl,cursor}*` (the OpenCL/VA-API/VDPAU *loaders* are bundled, see above);
   - glibc;
   - `libstdc++` and `libgcc_s`. Mesa's drivers load the host `libstdc++`, so an older bundled copy would break them. The cost: the host needs a glibc/libstdc++ at least as new as the build environment's, hence the Ubuntu 24.04 container (floor above).
   - the sound servers, D-Bus, fontconfig, freetype and udev.

   SDL3 and libepoxy dlopen the host's GL/EGL/Vulkan/Wayland/X11 at run time.
6. **Checks** (the build fails otherwise):
   - no GPU or C++ runtime library in `usr/lib`;
   - every `ou-*` survives `--help` without a signal (this catches broken patchelf output);
   - every binary and Qt plugin reaches `usr/lib` through its RUNPATH, with no `LD_LIBRARY_PATH`.
7. **AppRun** (`packaging/appimage/AppRun`) dispatches:
   - no argument, or `launcher` → `ou-launcher`;
   - `game|viewer|movie|audio|import|asset|text|sign` → `ou-<name>`;
   - `tools` → usage.
   It sets no `LD_LIBRARY_PATH`, so programs the launcher opens (file manager, browser) see a clean environment.

Our own binaries are stripped with the host `strip`. linuxdeploy's bundled strip cannot read `.relr.dyn` and fails on current Arch libraries, so `NO_STRIP=1` is set for it.

### Verification (2026-10-18)

All run from a copy in a temporary directory under `env -i HOME=<tmp> XDG_RUNTIME_DIR WAYLAND_DISPLAY DISPLAY [XAUTHORITY] PATH=/usr/bin:/bin`, i.e. no build directory on any path:

- [O] Size: 86 MB (124 bundled libraries).
- [O] `OU.AppImage game --game-dir "/path/to/Tomb Raider Underworld" --level 'pc-w\l4_pool.drm' --frames 30 --screenshot game.ppm --no-audio` (FUSE mount): exit 0. The screenshot shows Lara on the l4_pool stairs with textured stone and water; the overlay reports OpenGL 4.6, Mesa 26.2.4 radeonsi (host driver).
- [O] `OU.AppImage viewer --level --drm 'pc-w\l4_pool.drm' --renderer vulkan --frames 30 --screenshot …`: the level renders on RADV (Vulkan 1.4.354 host loader).
- [O] `--self-test`:
  - with `--appimage-extract-and-run` and `QT_QPA_PLATFORM=offscreen`: exit 0;
  - on the live Wayland session: exit 0. `LD_DEBUG=files` shows `libqwayland.so`, `libQt6WaylandClient.so.6` and `libxdg-shell.so` loaded from `/tmp/.mount_*/usr/lib/…`;
  - with `QT_QPA_PLATFORM=xcb` (XWayland): exit 0.
- [O] `OU.AppImage tools`, `sign` and `--version` (`ou-launcher 0.1.0`).
- [O] Other distributions: see "Old-glibc verification" above (Ubuntu 24.04 container). A host build (Arch) still needs a glibc/libstdc++ as new as Arch's.

## Signed release: `tools/release.sh [--dry-run] [--test]`

Before running it:
1. Bump `project(OpenUnderworld VERSION X.Y.Z)` in `CMakeLists.txt`.
2. Move `## [Unreleased]` entries in `CHANGELOG.md` to `## [X.Y.Z] - date`.
3. Commit and push to main.

The script:
1. Checks the preconditions:
   - the key exists with mode 0600 and lives outside the repository;
   - `HEAD == origin/main`;
   - tag and release do not exist yet;
   - `CHANGELOG.md` has the version's section.
2. Releases a clean detached `git worktree` of `HEAD` in `build-release/src` (a fixed path, so the container's Release build in `build-release/appimage-noble` stays configured and only rebuilds what changed since the last release). Uncommitted changes in your tree are listed and **excluded**; other people's work in progress never ships. Logs and the dist files go to `build-release/vX.Y.Z/`.
3. Tests are **off by default**: the maintainer hand-tests each AppImage. `--test` (or `OU_RELEASE_TESTS=1`) runs the full ctest in a Debug + ASan/UBSan build in `build-release/test` (real-data tests when `OU_GAME_DIR` is set; ~35 min).
4. Tags `vX.Y.Z` (annotated, local), then builds the AppImage from the worktree with `build-in-container.sh` (Docker is a precondition; without it the script stops with an error naming the Containerfile). The version is exactly `X.Y.Z` because `git describe` sits on the tag. The `== floor:` line of the build log fills the notes' *Requirements* line. It smoke-runs `--self-test` and `game --help` on the host; `ou-sign` is the AppDir's copy.
5. Makes the manifest: `ou-sign manifest --version X.Y.Z --channel stable --changelog <section> --base-url https://github.com/sergen213/openunderworld/releases/download/vX.Y.Z/ payload/`. The payload is the AppImage only, a project-owned file.
6. Signs and verifies:
   - `ou-sign sign --dir ~/.config/openunderworld-signing manifest.json`. A warning that the key differs from the compiled-in key aborts the release.
   - `ou-sign verify manifest.json` checks against the key compiled into the release build.
   - Key rotation (docs/UPDATES.md): `OU_ROTATE_FROM=<old public key hex>` signs with the old key in the signing dir while the build embeds the new `packaging/release-pubkey.hex`; the script then requires the manifest to verify with the old key and fail with the compiled-in one, and verifies the published manifest with `--pubkey <old>`.
   - `sha256sum -c`.
7. Pushes the tag and runs `gh release create vX.Y.Z --latest --title "OpenUnderworld X.Y.Z (experimental)"` with the notes (header + changelog section). Assets: the AppImage, `.sha256`, `.zsync`, `manifest.json` and `manifest.json.sig`.
8. Downloads every asset back into a temp dir, then runs `sha256sum -c`, `ou-sign verify` and `cmp` on the manifest.

On any failure the worktree is removed, as is the local tag if it was not pushed yet. `--dry-run` stops before step 7.

Heavy steps run under `systemd-run --user --scope -p MemoryMax=6G` when available (`OU_RELEASE_MEMMAX` overrides). Without the limit, the build can run the machine out of memory.

**Releases are normal GitHub releases marked "experimental" in the title and notes, not GitHub pre-releases.** `releases/latest/download/…` (the launcher's default manifest URL) never resolves to a pre-release. Observed: a repo with only pre-releases answers `/releases/latest` with a redirect to `/releases`, and the API returns 404.

### v0.1.0 (2026-10-18)

- [O] `OU_GAME_DIR="/path/to/Tomb Raider Underworld" tools/release.sh`:
  - ctest 122/122 passed on a clean worktree of `8b36983`;
  - built `OpenUnderworld-0.1.0-x86_64.AppImage` (90 098 168 bytes, sha256 `3c76f545…a99a133`);
  - signed and verified the manifest, then published <https://github.com/sergen213/openunderworld/releases/tag/v0.1.0> with the AppImage, `.sha256`, `.zsync`, `manifest.json` and `manifest.json.sig`;
  - the download-back check passed.

  The whole run took 104 s. A `--dry-run` before it passed the same way.
- [O] Assets downloaded again with `gh release download` into a fresh 0700 temporary directory and run under `env -i`:
  - `sha256sum -c` OK; `OpenUnderworld-0.1.0-x86_64.AppImage sign verify manifest.json` → `OK: version 0.1.0, channel stable, 1 files`;
  - `game … --level 'pc-w\l4_pool.drm' --frames 30 --screenshot`: Lara on the l4_pool stairs, screenshot viewed;
  - `viewer … --renderer vulkan`: the level renders on RADV, screenshot viewed;
  - `--self-test` passes both with `--appimage-extract-and-run` (offscreen) and on Wayland.
- [O] Self-update over the live release: see `docs/UPDATES.md` ("Live verification").

## CI

- `.github/workflows/ci.yml`: Arch container, Debug + ASan/UBSan, ctest, makepkg.
- `.github/workflows/appimage.yml`: on pushes to main, `v*` tags and manual dispatch. It runs `build-in-container.sh` (the same Ubuntu 24.04 Containerfile as releases) on the runner, smoke-tests with `smoke-in-container.sh` (self-test and `--version` in a clean `ubuntu:24.04`; no game data or GPU on CI), and uploads `build-appimage/out/*` as the artifact `OpenUnderworld-AppImage-<sha>`. These AppImages are **unsigned test builds**: there is no manifest and no GitHub release. Download one from the run page (Actions → appimage → Artifacts) or with `gh run download`.

[O] The first push of these workflows (`8b36983`) was green: `ci` in 3m42s and `appimage` in 1m55s. The CI AppImage `OpenUnderworld-0.1.0-git.8b36983-x86_64.AppImage` is 79 MB; it is smaller than the local build because the Arch container lacks some optional ffmpeg dependencies [I]. Its smoke step printed `ou-launcher 0.1.0`, and the artifact was uploaded (81.6 MB).