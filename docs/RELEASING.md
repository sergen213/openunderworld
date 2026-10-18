# Releasing

Status labels: [O] observed (run and seen), [I] inferred from the code, [U] unknown.

Official releases are **local and signed**: `tools/release.sh` on the maintainer workstation, which holds the Ed25519 key in `~/.config/openunderworld-signing/release.key`. CI builds unsigned test AppImages only; it never sees the key.

## AppImage: `packaging/appimage/build-appimage.sh [OUT_DIR]`

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
4. **Qt, bundled by hand.** `linuxdeploy-plugin-qt` is not installed on this workstation (`~/.local/bin` has none), and no unverified tool is downloaded. Only `ou-launcher` uses Qt (Widgets), so the script:
   - copies the plugins it needs from `qmake6 -query QT_INSTALL_PLUGINS` to `usr/lib/qt6/plugins/`: `platforms/{libqxcb,libqwayland,libqoffscreen}`, `wayland-shell-integration/libxdg-shell`, `wayland-decoration-client/{libbradient,libadwaita}`, `platformthemes/libqxdgdesktopportal`, `imageformats/{libqsvg,libqico,libqjpeg}`, `iconengines/libqsvgicon`;
   - lets linuxdeploy deploy their dependencies (`--deploy-deps-only`);
   - writes `usr/bin/qt.conf` (`Prefix = ..`, `Plugins = lib/qt6/plugins`).

   The plugins sit under `usr/lib/qt6/plugins` because linuxdeploy writes a fixed `$ORIGIN/../../../` RUNPATH for them; under `usr/plugins` that resolved to the AppDir root [O].
   No KDE/GTK platform theme is bundled (they would pull in KF6/GTK); the launcher uses Qt's Fusion style.
5. **Host-only libraries.** linuxdeploy's built-in excludelist plus an explicit list keep these out:
   - the GPU stack: `libGL*`, `libGLX*`, `libEGL*`, `libGLES*`, `libOpenGL*`, `libGLdispatch*`, `libvulkan*`, `libdrm*`, `libgbm*`, `libOpenCL*`, `libva*`, `libvdpau*`, `libnvidia*`, `libwayland-{client,egl,cursor}*`;
   - glibc;
   - `libstdc++` and `libgcc_s`. Mesa's drivers load the host `libstdc++`, so an older bundled copy would break them. The cost: the host needs a glibc/libstdc++ at least as new as the build host's (Arch rolling) [I].
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

### Verification on this machine (2026-10-18)

All run from a copy in `/tmp/ou-apptest.*` under `env -i HOME=<tmp> XDG_RUNTIME_DIR WAYLAND_DISPLAY DISPLAY [XAUTHORITY] PATH=/usr/bin:/bin`, i.e. no build directory on any path:

- [O] Size: 86 MB (124 bundled libraries).
- [O] `OU.AppImage game --game-dir "/mnt/1st4TB/Tomb Raider Underworld" --level 'pc-w\l4_pool.drm' --frames 30 --screenshot game.ppm --no-audio` (FUSE mount): exit 0. The screenshot shows Lara on the l4_pool stairs with textured stone and water; the overlay reports OpenGL 4.6, Mesa 26.2.4 radeonsi (host driver).
- [O] `OU.AppImage viewer --level --drm 'pc-w\l4_pool.drm' --renderer vulkan --frames 30 --screenshot …`: the level renders on RADV (Vulkan 1.4.354 host loader).
- [O] `--self-test`:
  - with `--appimage-extract-and-run` and `QT_QPA_PLATFORM=offscreen`: exit 0;
  - on the live Wayland session: exit 0. `LD_DEBUG=files` shows `libqwayland.so`, `libQt6WaylandClient.so.6` and `libxdg-shell.so` loaded from `/tmp/.mount_*/usr/lib/…`;
  - with `QT_QPA_PLATFORM=xcb` (XWayland): exit 0.
- [O] `OU.AppImage tools`, `sign` and `--version` (`ou-launcher 0.1.0`).
- [U] Other distributions: not tried. Expect failures on hosts with an older glibc/libstdc++ than Arch's.

## Signed release: `tools/release.sh [--dry-run]`

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
2. Releases a clean detached `git worktree` of `HEAD` in `build-release/vX.Y.Z/src`. Uncommitted changes in your tree are listed and **excluded**; other people's work in progress never ships.
3. Runs the full ctest in a Debug + ASan/UBSan build. Real-data tests run when `OU_GAME_DIR` is set.
4. Tags `vX.Y.Z` (annotated, local), then builds the AppImage from the worktree. The version is exactly `X.Y.Z` because `git describe` sits on the tag. It smoke-runs `--self-test` and `game --help`.
5. Makes the manifest: `ou-sign manifest --version X.Y.Z --channel stable --changelog <section> --base-url https://github.com/sergen213/openunderworld/releases/download/vX.Y.Z/ payload/`. The payload is the AppImage only, a project-owned file.
6. Signs and verifies:
   - `ou-sign sign --dir ~/.config/openunderworld-signing manifest.json`. A warning that the key differs from the compiled-in key aborts the release.
   - `ou-sign verify manifest.json` checks against the key compiled into the release build.
   - `sha256sum -c`.
7. Pushes the tag and runs `gh release create vX.Y.Z --latest --title "OpenUnderworld X.Y.Z (experimental)"` with the notes (header + changelog section). Assets: the AppImage, `.sha256`, `.zsync`, `manifest.json` and `manifest.json.sig`.
8. Downloads every asset back into a temp dir, then runs `sha256sum -c`, `ou-sign verify` and `cmp` on the manifest.

On any failure the worktree is removed, as is the local tag if it was not pushed yet. `--dry-run` stops before step 7.

Heavy steps run under `systemd-run --user --scope -p MemoryMax=6G` when available (`OU_RELEASE_MEMMAX` overrides). The workstation OOM-kills the terminal otherwise.

**Releases are normal GitHub releases marked "experimental" in the title and notes, not GitHub pre-releases.** `releases/latest/download/…` (the launcher's default manifest URL) never resolves to a pre-release. Observed: a repo with only pre-releases answers `/releases/latest` with a redirect to `/releases`, and the API returns 404.

### v0.1.0 (2026-10-18)

- [O] `OU_GAME_DIR="/mnt/1st4TB/Tomb Raider Underworld" tools/release.sh`:
  - ctest 122/122 passed on a clean worktree of `8b36983`;
  - built `OpenUnderworld-0.1.0-x86_64.AppImage` (90 098 168 bytes, sha256 `3c76f545…a99a133`);
  - signed and verified the manifest, then published <https://github.com/sergen213/openunderworld/releases/tag/v0.1.0> with the AppImage, `.sha256`, `.zsync`, `manifest.json` and `manifest.json.sig`;
  - the download-back check passed.

  The whole run took 104 s. A `--dry-run` before it passed the same way.
- [O] Assets downloaded again with `gh release download` into a fresh 0700 `/tmp` dir and run under `env -i`:
  - `sha256sum -c` OK; `OpenUnderworld-0.1.0-x86_64.AppImage sign verify manifest.json` → `OK: version 0.1.0, channel stable, 1 files`;
  - `game … --level 'pc-w\l4_pool.drm' --frames 30 --screenshot`: Lara on the l4_pool stairs, screenshot viewed;
  - `viewer … --renderer vulkan`: the level renders on RADV, screenshot viewed;
  - `--self-test` passes both with `--appimage-extract-and-run` (offscreen) and on Wayland.
- [O] Self-update over the live release: see `docs/UPDATES.md` ("Live verification").

## CI

- `.github/workflows/ci.yml`: Arch container, Debug + ASan/UBSan, ctest, makepkg.
- `.github/workflows/appimage.yml`: on pushes to main, `v*` tags and manual dispatch. It runs `build-appimage.sh` in an Arch container, smoke-tests `--self-test`/`--version` offscreen with extract-and-run, and uploads `build-appimage/out/*` as the artifact `OpenUnderworld-AppImage-<sha>`. These AppImages are **unsigned test builds**: there is no manifest and no GitHub release. Download one from the run page (Actions → appimage → Artifacts) or with `gh run download`.

[O] The first push of these workflows (`8b36983`) was green: `ci` in 3m42s and `appimage` in 1m55s. The CI AppImage `OpenUnderworld-0.1.0-git.8b36983-x86_64.AppImage` is 79 MB; it is smaller than the local build because the Arch container lacks some optional ffmpeg dependencies [I]. Its smoke step printed `ou-launcher 0.1.0`, and the artifact was uploaded (81.6 MB).