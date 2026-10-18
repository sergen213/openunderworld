# Existing local project audit (reuse candidates for OpenUnderworld)

Audit date: 2026-10-18. Scope: six sibling ports on this workstation, inspected **read-only** to find
generic infrastructure OpenUnderworld can reuse. No build, fetch, checkout or write was done in any of
these trees. Line counts are from `wc -l`.

Legend: **Observed** = seen in files or command output. **[INFERENCE]** = reasoned, not verified.
Reuse verdicts: *reuse as-is*, *adapt*, *reference only*, *not applicable*.

Method note: the first `git status` pass ran without `GIT_OPTIONAL_LOCKS=0`, so git may have refreshed
the stat cache in `.git/index` of the four git repos. That touches only git metadata, not tracked
content. Every later git command used `GIT_OPTIONAL_LOCKS=0`.

## 0. Licensing gate (read first)

| Project | Own-code licence (observed) | Copying into a GPL-3.0-or-later tree |
|---|---|---|
| Kabus 22 | **None.** `README.md:258`: "No project-wide license has been assigned." No SPDX headers. `native/assets.cpp` carries an MIT notice for parts derived from libgamestudio. | Not allowed until the author grants a licence. |
| etiyami | **None.** No LICENSE file, no SPDX headers, no README statement. | Not allowed until the author grants a licence. |
| Pusu: Uyanış | **GPL-3.0-only**: `LICENSE` is the GPLv3 text, and `README.md:130` says "GNU GPL version 3 only". | Compatible, but the combined work becomes GPL-3.0-only unless the author relicenses as "-or-later". |
| IKV | **None.** No LICENSE file, no SPDX headers, no README statement. | Not allowed until the author grants a licence. |
| CitroenC4Robot | **None.** `README.md:674` says licensing still "require[s] review". | Not allowed until the author grants a licence. |
| SuperCan | **None.** | Not allowed until the author grants a licence. |

All git commits in the four repos are by `sergen213 <20866366+sergen213@users.noreply.github.com>`, and
`gh` on this machine is logged in as `sergen213`. **[INFERENCE]** That makes the OpenUnderworld owner the
copyright holder of all six projects. If so, they can relicense any file they copy, for example by adding
`SPDX-License-Identifier: GPL-3.0-or-later` together with a provenance line in the commit message.
**Open question for the owner:** confirm sole authorship and grant GPL-3.0-or-later for the files
actually copied. Until then, treat everything below as *reference only* for copying purposes.

Vendored third-party code inside these trees that is GPL-3-compatible on its own merits:
- AMD FidelityFX SDK v1.1.4 / FSR 3.1.4 subset (MIT).
- stb_vorbis (public domain or MIT).
- ODE 0.16.6 (BSD option).
- Xiph libvorbis 1.3.7 (BSD-3).
- uruntime 0.8.1 (MIT; its helpers include LGPL/GPL code).
- innoextract 1.9 (zlib).

## 1. Kabus 22 — `/mnt/1st4TB/Kabus 22`

**Repository state**
- Mount: `/mnt/1st4TB`, `/dev/sda1`, btrfs.
- git: branch `main`, clean (0 changes), 114 commits, remote `github.com/sergen213/kabus22-linux`.
- Latest tag `v0.4.8`, HEAD `9625182 release: 0.4.8` (2026-10-17). Earlier commits:
  - `edb4416 docs: second RT perf pass…`
  - `e948829 vulkan: fix pre-existing validation errors…`

**What it is**
- Native Linux engine for *Kabus 22* (Conitec 3D GameStudio A6/A7). It executes the original WDL scripts.
- Also a GTK3 installer, a GTK3 launcher and an Ed25519-signed updater.
- Languages: C++20, GLSL, Python 3, Java (Ghidra scripts), sh.
- Build: CMake ≥3.20 + Ninja. Root `CMakeLists.txt` plus a separate `installer/CMakeLists.txt`.

**Dependencies**
- Engine: **SDL2** (not SDL3), epoxy, FFmpeg libs, freetype, fontconfig.
- Vulkan headers only; the loader is `dlopen`ed.
- Installer: GTK3, libarchive, curl, OpenSSL, nlohmann_json.

**Docs**
- `README.md`.
- `docs/`: RAYTRACING, GRAPHICS_ROADMAP, INSTALLER, RELEASE_PROCESS, TEXTURE_RESTORATION, REVERSE_ENGINEERING, and others.
- `docs/reverse-engineering/` (FUNCTION_MAP and others).

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| Signed self-update | `installer/update.{hpp,cpp}` (49/393), `installer/updater.cpp` (187), `installer/layout.cpp` (221) | Ed25519 via OpenSSL `EVP_PKEY_new_raw_public_key` + `EVP_DigestVerify`. The signature is checked over the exact manifest bytes *before* JSON parsing. The public key is set at build time (`-DK22_UPDATE_PUBLIC_KEY`). Manifest has format, version, channel, min_glibc, payload {name, sha256, size}. HTTPS-only libcurl. libarchive extraction rejects absolute paths, `..`, links, devices and setuid bits. Install layout is `versions/<v>` with atomic symlink swap and rollback, under `flock` and an ownership marker. | adapt (rename `kabus22` identifiers) |
| Release signing | `tools/release/make_update_manifest.py` (126), `tools/release/sign_manifest.py` (66) | Deterministic tar.gz (sorted, root-owned, mtime 0). Refuses files that look like game assets. Ed25519 signing via Python `cryptography`. | adapt |
| Update tests | `installer/check_update.cpp` (242), `installer/check_updater_e2e.py` (221) | Offline unit tests, plus an end-to-end test against a test-only build that accepts `http://127.0.0.1`. Also checks the release binary lacks those test hooks. | adapt |
| AppImage | `tools/package_kabus_appimage.py` (933), `tools/Dockerfile.ubuntu24` (140), `tools/build_kabus_baseline.sh` (91), `packaging/runtime-uruntime/` | Type-2 AppImage on a SHA-pinned uruntime. Host GPU/libc libraries are excluded by regex; uses patchelf; builds a corresponding-source sidecar. Builds run in a container with read-only source, `--cap-drop=ALL` and no network. | adapt |
| CI | `.github/workflows/ci.yml` (39), `.github/workflows/release.yml` (88) | `ci.yml`: Ubuntu 24.04, Ninja build, ctest (fixture tests self-skip). `release.yml`, on a `v*` tag: version consistency check, signing-secret vs embedded-key check, build, sign manifest, `gh` **draft** release. | adapt |
| Vulkan loader | `native/vulkan_loader.{hpp,cpp}` (117/34) | X-macro function list + `dlopen("libvulkan.so.1")` with `VK_NO_PROTOTYPES`, so a GL-only host needs no libvulkan. | reuse as-is (rename) |
| SPIR-V embedding | `native/embed_spirv.py` (35) + CMake rules | glslang output → `constexpr uint32_t[]`. | reuse as-is |
| FSR 3.1.4 on Linux/Vulkan | `native/fsr_vk.cpp` (243), `tools/k22_fsr3_blobs.py`, `third_party/FidelityFX-SDK/` (MIT, `KABUS-PROVENANCE.txt`) | Port taken from IKV, with RADV fixes. | adapt; prefer IKV's copy (the origin) |
| Vulkan RT (ray queries) | `native/render_vulkan*.cpp` (712/891/321), `native/shaders/{ray_query.glsl 127, rt_signals.comp 218, rt_filter.comp 78}` | BLAS/TLAS, ray-query shadows/AO/GI/reflections, temporal + à-trous denoise. Material struct is A6-shaped. | reference / adapt structure |
| OpenGL | `native/render_gl.cpp` (1928) | GL 3.3 via epoxy, deeply A6-specific. Has a `glReadPixels` → PPM capture. | reference only |
| PE parsing | `native/pe_font.hpp` (228) | Bounds-checked PE32 section-table parser with self-tests. The font fingerprints in it are game-specific. | reuse the parser part |
| Ghidra | `reverse/scripts/RecoverKabus.java` (297), `reverse/scripts/ExportEmbeddedPcx.java` (300) | Read-only headless export of functions selected by a relevance regex (string/import seeds). Per-function timeout; writes a manifest of timeouts and omissions. | adapt (the regex is game-specific) |
| Reference comparison | `tools/wine-reference/wineref.py` (236), `tools/wine-reference/compare.py` (115) | Runs the original exe under Wine on a private Xwayland, drives it with XTest, captures with ffmpeg x11grab, scores with SSIM (masks/thresholds, diff montage). | adapt |
| Scenario harness | `native/scenario.cpp` (333), `tests/scenarios/run.cmake`, `tests/levels/sweep.cmake` | Fixed 1/60 s step, timed input, assertions, PPM captures, private XDG per run. Script semantics are WDL. | reference (concept) |
| Virtual gamepad | `tools/virtual-gamepad/vpad.py` (210) | uinput Xbox 360 pad with force-feedback logging. | reuse as-is |
| Misc helpers | `native/saves.cpp` (116): `write_atomic` (fsync + rename + dir fsync); `native/texture_pack.cpp` (463): texture-replacement packs keyed by FNV-1a | — | adapt |
| Repo hygiene | `.gitignore` (30 KB allowlist: ignore everything, then list reviewed sources) | Keeps game data out of git. | reuse the pattern |

Absent here: a DXT/BCn decoder (DDS goes through FFmpeg `AV_CODEC_ID_DDS`), SDL3, Qt, an Xvfb harness.

**Game-specific (not reusable)**
- WDL compiler and runtime: `native/wdl.cpp`, `native/engine.cpp`.
- WMB/MDL/HMP/WRS loaders: `native/assets.cpp`, `tools/wrs2.py`.
- Physics and ODE patches; co-op, doors and console code.
- K22 save formats.

**Copyrighted content (never copy)**
- The ISO folder and `.zip`, `setup.exe`, `setup-1.bin`, `Kabus22Patch/`, the patch `.7z`, `Drivers/`.
- `reverse/{decompiled,extracted,media,rea,ghidra}`.

## 2. etiyami — `/mnt/1st4TB/etiyami`

**Repository state**
- Mount: `/mnt/1st4TB`, `/dev/sda1`, btrfs.
- git: branch `main`, 18 commits, remote `github.com/sergen213/etiyami`, tags `v1.1.1` and `v1.1.0`.
- HEAD `ee07a4e docs: record public v1.1.1 installation and upgrade proof`.
- **36 uncommitted entries**: `CMakeLists.txt`, `README.md`, `REVERSE_ENGINEERING.md`, `native/*`, and others. README has "UNRELEASED" sections on FSR 3.1.4 replacing TAA. **[INFERENCE]** That is work in progress; do not lift unreleased files.

**What it is**
- Native C++20/SDL3 rebuild of *ETI Yami* (2005).
- OpenGL/GLES renderer via epoxy; optional Vulkan 1.3 renderer with ray queries + FSR 3.1.4.
- SDL3 launcher, ISO→install GUI, uninstaller, self-updater, AppImage and `.run` packaging.
- Build: CMake ≥3.20 (`CMakeLists.txt`, 317 lines).

**Dependencies**
- pkg-config: sdl3, epoxy, FFmpeg libs, libxml-2.0, libcurl, libarchive, json-c.
- Vulkan headers + glslang/glslc at build time only.

**Docs:** `README.md` (357 lines), `REVERSE_ENGINEERING.md` (462 lines), `third_party/fidelityfx-sdk/README.md` (patch log).

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| Update transaction (unsigned) | `native/update.cpp` (636), `native/update_install.cpp` (697), `native/updater_main.cpp` (76) | Trust root is GitHub TLS plus the API `digest` field; **no publisher signature**. Redirects pinned to `api.github.com:443`. Installed helper performs a stage/backup rename transaction with reverse rollback. | adapt the transaction only; signing must come from elsewhere |
| Install ownership / uninstall | `native/install_ownership.hpp` (118), `native/uninstall_main.cpp` (634) | Owned-file inventory markers; the remover deletes only inventoried paths. | adapt |
| Packaging | `tools/package_release.py` (517), `tools/package_setup.py` (475), `tools/package_appimage.py` (598) | ELF dependency closure with host-library exclusion regex and max-glibc check. Self-extracting `.run` with hashed payload header. Reproducible `mksquashfs` on uruntime. `--self-test` mutation checks. | adapt |
| CI (release only) | `.github/workflows/release.yml` (302) | Tag → x86_64/arm64 matrix on ubuntu-22.04 (glibc 2.35 baseline); SHA-pinned SDL3/FFmpeg/curl built from source; draft release, then re-verify GitHub digests, then publish. Actions pinned by SHA. | adapt |
| Vulkan dispatch | `native/vulkan_api.hpp` (123) | X-macro tables loaded through SDL's `vkGetInstanceProcAddr`; no libvulkan link. | reuse as-is (rename) |
| Shader embedding | `tools/embed_spirv.py` (124), `tools/embed_fsr_shaders.py` (292) | Validates SPIR-V magic and size; atomic replace; `--self-test`. | reuse as-is (rename namespace) |
| Vulkan RT + FSR | `native/vulkan_renderer.cpp` (977), `native/vulkan_effects.cpp` (1117), `native/shaders/*`, `third_party/fidelityfx-sdk/yami/*` | Dynamic rendering + sync2, BLAS/TLAS, ray-query effects, GPU timestamp stages. Shaders are tied to Yami's material model. | reference / adapt plumbing |
| Renderer tests | `native/check_vulkan.cpp` (2036), `native/check_renderer.cpp` (477) | Synthetic meshes and textures, pixel asserts on `capture_rgba()`. CTest labels `native-display`/`vulkan`. No Xvfb script. | adapt the pattern |
| Ghidra | `analysis/RecoverEngine.java` (80), `analysis/TraceStartup.java` (56) | Bulk decompile to `functions/<va>.c`, `functions.tsv`, `calls.tsv`, `disassembly.txt`, `strings.tsv`. Hardcoded rename table and API regex. No headless wrapper. | adapt |

Absent here: signed updates, Qt, DXT/BCn.

**Game-specific (not reusable):** Yami formats, script VM, gameplay, AI, menus; `extract_game.py` (MSI/CAB specific to this release); `prepare_reference.py` and `check_activation.c` (hardcoded VAs).

**Copyrighted content (never copy):** `game/`, `Yami.msi`, `Data1.cab`, `setup.exe`, `instmsi*.exe`, `.wine-yami/`, `analysis/original/`, `analysis/recovered/`, the Ghidra `.rep` databases, screenshots containing game art.

## 3. Pusu: Uyanış — `/mnt/1st4TB/Pusu: Uyanış`

**Repository state**
- Mount: `/mnt/1st4TB`, `/dev/sda1`, btrfs.
- git: branch `main`, 6 commits, remote `github.com/sergen213/pusu-uyanis-linux`.
- HEAD `c17913b docs: link verified rendering and loading bugfix release`.
- **15 uncommitted entries**, including the untracked `src/updater.{cpp,hpp}` and `checks/updater.cpp`.

**What it is**
- Native C++20 reimplementation of *Pusu: Uyanış* (a Quake3-style BSP engine).
- Importer for CUE/BIN, ISO, MSI and CAB.
- GTK3 installer and launcher; OpenGL 3.3 (GLEW) and Vulkan 1.2 renderers with optional ray queries.
- Build: CMake ≥3.20, Ninja required because the path contains `:`.

**Dependencies**
- **SDL2** (not SDL3), GTK3, libarchive, curl, json-c, FFmpeg, libsquish, libmsi/libgcab, OpenSSL.

**Licence:** GPL-3.0-only (see §0). `packaging/art/*.png` are derived from game artwork and have no public licence.

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| Updater (unsigned, uncommitted) | `src/updater.cpp` (367), `checks/updater.cpp` (74) | Hardened libcurl transport: https on port 443 only, host allowlist, manual redirect re-validation, size caps, deadline. Strict json-c parsing. Trust is the GitHub API `digest`; **no signature**. It executes the downloaded AppImage with `--appimage-extract` after the hash check. | reference (transport hardening); don't copy the execute-to-extract step |
| Atomic publish / import | `src/importer.cpp` (1201) | `renameat2` NOREPLACE/EXCHANGE publication with identity revalidation. Symlink-free path checks; NFC case-fold collision detection. Ownership manifest. libarchive extraction with links forbidden. Activity lock (`flock`) between game and updater. | adapt |
| AppImage + licence closure | `packaging/build_appimage.py` (472), `packaging/license_bundle.py` (1033), `packaging/check_packaging.py` (184), `packaging/sources/public-runtime/build.sh` | Dependency closure plus a licence and corresponding-source bundler. Reproducible static type-2 runtime build with an LGPL relink kit. | adapt (GPL-3.0-only code) |
| Shader embedding | `cmake/embed_spirv.cmake` (29) | Pure-CMake SPIR-V → `constexpr std::array` with a magic check. | reuse as-is (no Python needed) |
| CLI smoke test | `checks/game_cli.py` (90) | Runs with no display and no data (nonexistent `SDL_VIDEODRIVER`), isolated HOME/XDG. Checks the `--capabilities` JSON and that a bad settings file is never rewritten. | adapt |
| Vulkan RT | `src/renderer_vulkan{,_raster,_rt,_quality}.cpp` (611/527/423/405), `src/shaders/vulkan/ray_query.glsl` | Frame slots, upload batches, `readback()`; BLAS/TLAS with shared scratch. Tied to Q3-style materials. | reference only |
| Ghidra (untracked) | `.work/native-analysis/NativeGameEvidence.java` (90) | Breadth-first callee walk to depth N from seed addresses; xref (`%addr`) and memory (`~addr+len`) dumps; per-function decompile + listing. | adapt (needs relicence; GPL-3.0-only) |
| Reference / observer harnesses (untracked) | `.work/diagnosis/run_diagnosis.py` (60), `.work/native-gl-proof/sdl_input_driver.cpp` (642) | The original exe under `bwrap --unshare-all` + `xvfb-run` + Wine. An LD_PRELOAD SDL input-injection and back-buffer capture shim. | reference |

Absent here: CI, Qt, SDL3, signed updates, BCn decoding (the Vulkan path *encodes* BC1/3 with libsquish).

**Game-specific (not reusable):** `src/game*.cpp`, `script_vm.cpp`, `assets.cpp`, `resources.cpp`. **`src/resources.cpp:176-211` embeds a game-protection AES key — never copy.**

**Copyrighted content (never copy):** `Pusu-Uyanis_Win_TR_CD/`, `Pusu-Uyanis_Fix_Win_TR_Unofficial-Patch/`, `.work/{disc,game,native-import,transform/plaintext,native-analysis/**/<addr>.txt,reference,package}`, `packaging/art/*.png`.

## 4. IKV — `/mnt/2nd4TB/IKV`

**Repository state**
- Mount: `/mnt/2nd4TB`, `/dev/sdb1`, btrfs.
- git: branch `main`, clean, 9 commits, remote `github.com/sergen213/ikv-linux`, tag `v0.2.0`.
- HEAD `4101f39 IKV Linux 0.8.0: Starter gear, NPC names…` (2026-10-17).

**What it is**
- Clean-room Linux port of the MMO *İstanbul Kıyamet Vakti*.
- C++20 client: **SDL3** + OpenGL 3.3 core, or Vulkan 1.2 with ray queries + FSR 3.1.4.
- HTTPS auth service and TLS world server on PostgreSQL.
- **Qt6 launcher** with **Ed25519-signed** self-update.
- Build: CMake ≥3.20 (`CMakeLists.txt`, version 0.8.0); `IKV_ENABLE_SANITIZERS` option.

**Dependencies:** SDL3 ≥3.4, OpenSSL ≥3.2, Qt6 Core/Widgets/Network, libarchive, Vulkan + glslc, Freetype, fontconfig, turbojpeg, Boost headers, nlohmann_json, PostgreSQL.

**Docs:**
- `README.md`.
- `docs/`: LAUNCHER_UPDATES, GRAPHICS_ROADMAP, REVERSE_ENGINEERING, ASSET_FORMATS, FUNCTION_MAP, SELF_HOSTING, …
- `docs/network/`.

**No CI** (no `.github/`); releases are built and signed locally.

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| **Signed self-update** | `include/ikv/updates.hpp` (51), `src/launcher_updates.cpp` (686), `src/update_helper_main.cpp` (40) | Detached 64-byte Ed25519 signature over the exact manifest bytes, verified with OpenSSL `EVP_DigestVerify`. The trust PEM is embedded at configure time. Manifest schema 2: version, channel, **monotonic sequence**, package {size, sha256}, files[] with sha256 and executable flag. Parser rejects duplicate keys; enforces depth and size limits. libarchive extraction rejects links/xattrs/unlisted entries. Install layout is `releases/<v>` with `current`/`previous` symlinks, an fsync'd transaction journal, a per-channel anti-rollback highwater (`.update-highwater`), and `renameat2(RENAME_EXCHANGE)` rollback. Uses `flock` and owner/permission checks on ancestors. Linux-only. | **adapt** (rename only) — best updater core of the six |
| Signing / release tools | `tools/sign_manifest.py` (247), `tools/release.py` (215) | Canonical JSON (sort_keys, compact) signed with `openssl pkeyutl -sign -rawin`. Release flow: build AppImage → smoke → deterministic tar → sign → self-install proof → advance sequence → `gh release`. Hardcoded repo and key path. | adapt |
| Update tests | `tests/update_integration.py` (799), `tests/update_version_test.cpp` | Disposable keys, local HTTPS server, malicious-archive cases, offscreen Qt. | adapt |
| Qt launcher | `src/launcher_main.cpp` (1177) | GitHub releases client via `QNetworkAccessManager` with host allowlist and transfer budgets; drives `ikv-updater`. Turkish MMO UI. | adapt the update client logic; UI reference only |
| Qt GUI smoke test | `tests/launcher_gui_smoke.cpp` (1140) | Compiles `launcher_main.cpp` with `main` renamed; drives widgets via `findChild`; PNG grabs. | reuse the technique |
| PE resource extraction | `src/launcher_skin.cpp` (107) | Bounds-checked PE `RT_BITMAP` → BMP. | reuse as-is |
| AppImage | `tools/package_appimage.py` (310), `tools/appimage/Dockerfile.ubuntu24` (101), `packaging/runtime-uruntime/` | Container build with read-only source; DT_NEEDED closure with excludelist + host GPU regex; Qt plugins (xcb/wayland/offscreen); AppRun multiplexer; `--self-check`; JSON licence report. | adapt (drop PostgreSQL/Boost) |
| Renderer interface + math | `include/ikv/renderer.hpp` (303) | Abstract `Renderer` (render, make_resident/evict, resize, screenshot, upscaling). Camera VP for GL vs Vulkan clip space, frustum planes, sphere culling, texel-snapped shadow matrix. | reuse the math; interface as reference |
| Vulkan RT | `include/ikv/vulkan_rt.hpp` (67), `src/vulkan_rt.cpp` (1434), `shaders/vulkan/rt_lighting.glsl` (216), `shaders/vulkan/temporal.frag` | Ray-query RT: BLAS/TLAS, in-frame refit for skinned meshes, budgeted background builds, alpha-tested hits via a 64-slot texture array, sun shadows + 1-bounce GI + reflections. Coupled to `ikv::Scene`. | adapt (swap the scene type) — most transferable RT code |
| Vulkan / GL backends | `src/renderer_vk.cpp` (2592), `src/renderer_gl.cpp` (1479) | Native BC1–3 upload on GL and VK; screenshot readback; frame-generation presenter thread. Game lighting is woven in. | reference only |
| FSR 3.1.4 port | `src/fsr_vk.cpp` (243), `tools/ffx_fsr3_blobs.py` (174), `third_party/FidelityFX-SDK/` (MIT, `IKV-PROVENANCE.txt`), `third_party/ikv-ffx-posix/` | Origin of the Kabus copy. Replaces the Windows-only FidelityFX_SC with glslc + reflection. | reuse (FFX: MIT; glue: needs relicence) |
| DDS / BCn | `src/assets.cpp` (650; DDS part only) | `decode_dds`/`decompress_dds`: DXT1/3/5 + masked RGB, all mips, pitch handling, size cap. Recognises ATI1/2 and BC4/5 FourCCs but rejects them; DX10 headers rejected. The same file holds the game-specific `.DAT` reader. | adapt (split out) |
| Image writer | `src/image_write.cpp` (57) | RGB → JPG/TGA/BMP/PPM. | reuse as-is |
| Asset import / verify / repair | `src/launcher_assets.cpp` (813), `src/launcher_extraction.cpp` (252) | Hash-manifest import, verify and repair (openat, O_NOFOLLOW). innoextract under resource limits; hardcoded installer hash. | adapt |
| RE tooling | `tools/ghidra/ExportEvidence.java` (84), `tools/ghidra/GraphicsEvidence.java` (104), `tools/ghidra/AssetReturnTypes.java` (282), `tools/pe_inventory.py` (104) | Evidence export (imports/IAT xrefs, strings with file offsets, seeds + one caller layer, cap 300 decompiles). D3D vtable-offset CALL finder. Stdlib PE inventory. | adapt / reuse `pe_inventory.py` |
| Test math | `tests/render_math_test.cpp` (155) | Renderer math unit test. | reuse |

**Xvfb recipe:** `docs/network/NETWORK_TESTING.md:102` documents one owned `Xvfb -displayfd 1 … -nolisten tcp`. It warns against parallel `xvfb-run -a`, which raced onto one display. `MESA_VK_WSI_DEBUG=sw` lets Vulkan present on Xvfb (gitignored `analysis/final_launcher_smoke.py`).

**Game-specific (not reusable):** `src/world*.cpp`, `server_main.cpp`, `auth_main.cpp`, `protocol.cpp`, `original_wire.cpp`, `client_*.cpp`, `scene*.cpp`, `adf_codec.cpp`, the `.DAT` reader, `shaders/vulkan/lighting.glsl`, `config/`, `database/`, `web/`, `deploy/`. Also `packaging/ikv-release-public.pem` — generate a new key.

**Copyrighted content (never copy):** `istanbulkur6_0.exe`, `original/`, `analysis/` (Ghidra projects, decompiled dumps), `.tools/`.

## 5. CitroenC4Robot — `/mnt/2nd4TB/CitroenC4Robot`

**Repository state**
- Mount: `/mnt/2nd4TB`, `/dev/sdb1`, btrfs.
- **Not a git repository** (`git status` fails at the filesystem boundary), and the scout found no nested `.git` at depth 3. It does contain `.github/workflows/native.yml`. **[INFERENCE]** A repo was planned or exported elsewhere.

**What it is**
- C++20 rebuild of *Citroën C4 Robot* (Acknex/3DGS, D3D9).
- **SDL3** + OpenGL runtime; Vulkan 1.3 renderer with ray queries.
- **Qt6 launcher** and **Qt6 Ed25519-signed updater**.
- Build: CMake ≥3.24, `-ffp-contract=off` (matches the original float rounding).

**Dependencies:** SDL3 ≥3.2, Vulkan + glslc, Qt6 (Widgets/OpenGL/Concurrent/Core/Network), OpenSSL, libarchive, libsndfile, minimal LGPL FFmpeg.

**Docs:** `README.md` (674 lines), `docs/{ENGINE_ARCHITECTURE,PROGRESS,KNOWN_ISSUES,FUNCTION_MAP,REVERSE_ENGINEERING,GRAPHICS_ROADMAP}.md`, `Goal`.

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| **Signed updater (Qt)** | `updater/src/main.cpp` (677), `updater/src/runtime_policy.hpp`, `updater/tests/safety.py` (273) | Ed25519 over raw `manifest.json` bytes, checked before parsing. Operator-configured trust PEM located outside the install root. Manifest: schema, version, os, arch, package, files[]. Size limits; GitHub host allowlist with manual redirects; TLS ≥1.2; JSON progress events; journaled `current`/`previous` activation. CLI `--check/--download/--install/--rollback/--verify-current`. | adapt |
| Packaging producer | `packaging/package_native.py` (281), `packaging/appimage/build.py` (96), `packaging/appimage/acquire.py` (144), `packaging/appimage/tools.lock.json`, `packaging/appimage/AppRun` | Deterministic USTAR + gzip from SOURCE_DATE_EPOCH; canonical JSON manifest; `openssl pkeyutl -sign -rawin`, then immediate verify. Reproducible appimagetool build. SHA-pinned tool acquisition. | `acquire.py`: reuse as-is; others: adapt |
| CI | `.github/workflows/native.yml` (214) | SHA-pinned actions; SDL3 3.4.18 built from a hashed tarball when apt's is too old; unittest + ctest. On tags: package twice and `cmp` for reproducibility, verify signatures, smoke-test the AppImage, then `gh release`. | adapt — the most complete CI of the six |
| Qt launcher | `launcher/src/main.cpp` (1252), `launcher/src/hardware.cpp` (410), `launcher/src/assets.cpp`, `launcher/src/quality_controls.cpp`, `launcher/src/integration.cpp` | Tabs; `QSaveFile` config; drives the updater as a subprocess over a JSON event protocol. `hardware.cpp` probes Vulkan devices (VRAM, `VK_KHR_acceleration_structure`/`ray_tracing_pipeline` bits) and GL via `QOffscreenSurface`. ctest `launcher-smoke` uses `QT_QPA_PLATFORM=offscreen`. | `hardware.cpp`: reuse as-is; launcher: adapt structure |
| Vulkan dispatch / cache | `src/rendering/vulkan/VulkanDispatch.hpp` (333), `src/rendering/vulkan/VulkanPipelineCache.hpp` (198) | X-macro dispatch, including ray-query functions. On-disk `VkPipelineCache` under XDG with a validated envelope and atomic write. | reuse (parametrize the shader list) |
| Vulkan RT | `src/rendering/vulkan/raytracing.cpp` (1315), `shaders/rt.comp`, `shaders/temporal.comp` | Ray-query compute effects, BLAS/TLAS refit, à-trous denoise, ray budgets. Scene-coupled. | reference / adapt |
| BCn + DDS | `src/assets/GpuImageBc.cpp` (336), `src/assets/Images.cpp` (766) | BC1–3 decode and encode, bit-matched to D3DX. Hardened DDS (DXT1/3/5, masked RGB, mips) plus BMP/TGA/PCX. No DX10 header, no BC4/5. | adapt |
| Smoke tests | `tests/test_native_window.py` (184), `tests/test_vulkan_native.py`, `tools/benchmark_native.py` | Real binary under private Xvfb (`xvfb-run`, `LIBGL_ALWAYS_SOFTWARE=1`, `SDL_VIDEODRIVER=x11`). PPM capture checks. Forces a missing Vulkan ICD to test recorded GL fallback. Rejects single-colour frames. | adapt — direct template for OpenUnderworld's GL smoke test |
| Ghidra | `tools/ghidra/analyze.py` (153), `tools/ghidra/ExportEvidence.java` (222) | Persistent project with per-binary `<name>_<sha12>` folders. First run imports; later runs use `-process -noanalysis`. Invocation ledger and SHA guard. TSV exports: functions, calls, imports, exports, strings + xrefs, data refs, x87/SSE control words, memory blocks. `define:ADDR`. | adapt (drop the Acknex entity-offset scans) |
| Inventory / import | `tools/inventory.py` (166), `tools/import_assets.py` (519) | Hash + `objdump` inventory with PE detection. Safe import from an Inno installer or install directory into a fresh output dir, with `O_NOFOLLOW` walks. | `inventory.py`: reuse; importer: adapt |
| Original-game observer | `tools/observe_original.py` (914) | bwrap sandbox + private Xvfb + disposable Wine prefix with no network; x11grab capture; input replay. | adapt skeleton |

**Game-specific (not reusable):** `src/game/**`, `src/physics/**`, `src/assets/{World,Meshes,Animation,LegacyModels}.cpp`, `Resources.cpp` (WRS), the material/lighting GLSL, `VulkanScene.cpp`, `SourceComposite.cpp`, `tools/original-assets.json`, `docs/FUNCTION_MAP.md`.

**Copyrighted content (never copy):** `Citroen-C4-Robot_Win_TR_Installer.exe`, `work/original/`, `work/ghidra/`, `work/reports/**/decompiled`, `work/reference/`, `work/verification/` (contains original-derived data).

## 6. SuperCan — `/mnt/2nd4TB/SuperCan`

**Repository state**
- Mount: `/mnt/2nd4TB`, `/dev/sdb1`, btrfs.
- **Not a git repository**; no nested `.git` found.

**What it is**
- Partial reconstruction of *SüperCan* (Acknex A8, D3D9/D3DX, PhysX, FMOD). Not playable.
- Asset import/verify, format parsers, SDL3/GL 3.3 scene inspector, D3D9 fixed-function emulator on GL, Vorbis + mixer.
- Build: CMake ≥3.20, Linux-only.
- Dependencies: SDL3 ≥3.4, OpenSSL, OpenGL, libogg; vendored libvorbis 1.3.7 (BSD-3).

**Docs:** `docs/{ASSET_FORMATS,ENGINE_ARCHITECTURE,FUNCTION_MAP,GRAPHICS_ROADMAP,INSTALLER_ARCHITECTURE,KNOWN_ISSUES,PROGRESS,REVERSE_ENGINEERING,SOURCE_COMPARISON}.md`, `docs/manifests/`, `Goal`.

**No** Vulkan, RT, Qt, updater, AppImage or CI.

**Generic infrastructure**

| Area | Files (lines) | Notes | Verdict |
|---|---|---|---|
| D3D9 shader bytecode | `src/assets/d3d9_shader.cpp` (493), `src/assets/d3d9_glsl.cpp` (606), `src/assets/effect.cpp` (304) | Bounds-checked SM1–3 VS/PS bytecode parser (CTAB, preshaders). D3D9 → GLSL 330 lowering with explicit sampler, semantic and clip conventions. `FEFF0901` effect parser. | adapt — **highly relevant [INFERENCE]**: TRU is a D3D9 game; its shader format is not yet identified |
| D3D9 → GL state | `src/render/d3d9_device.cpp` (740) | `IDirect3DDevice9` subset on GL 3.3. Depth `z' = 2z − w`, half-pixel offset, Y flip, winding mirror. Unmapped states throw. | reference (conversion rules) |
| DDS / DXT | `src/assets/image.cpp` (253) | DXT1 (incl. 1-bit alpha), DXT3, DXT5, masked RGB → RGBA8; size limits. Top mip only; no BC4/5 or DX10. | adapt |
| Hardened import | `src/assets/main.cpp` (671) | `openat` + O_NOFOLLOW per path component; SHA-256 with a replace-during-read check; casefold collision detection; innoextract via `posix_spawnp`. | adapt |
| Inspector / capture | `tools/asset-inspector/scene.cpp` (1382) | SDL3 + GL 3.3 RAII window; `glReadPixels` → BMP; `--frames N`; synthetic shader self-checks rendered into 4×4 FBOs. | adapt the scaffolding |
| Ghidra | `tools/ghidra/ExportEvidence.java` (174), `tools/ghidra/ExportVtables.java` (45), `tools/ghidra/ExportMachineEvidence.java` (132), `tools/ghidra/ExportPrerequisites.java` (54), `tools/ghidra/ExportFloatingEnvironment.java` (118) | JSONL exports (functions with callers/callees, symbols, imports, strings, xrefs); review-merge renames. RTTI vftable slots. Per-function instructions and bytes. FPU control-word inventory. | `ExportVtables` / `ExportMachineEvidence` / `ExportPrerequisites` / `ExportFloatingEnvironment`: reuse as-is; `ExportEvidence`: adapt |
| PE / inventory | `tools/ghidra/inspect_containers.py`, `tools/validation/source_inventory.py` | PE32 sections, overlay and certificate; portable-path checks. | reuse |
| Test conventions | `CMakeLists.txt` | Asset-dependent tests exit 77 with `SKIP_RETURN_CODE 77`; assert tests built with `-UNDEBUG`. Wine oracle probes call original DLL exports under `bwrap` (`analysis/reference/*_probe.c`). | reuse the pattern |

**Game-specific (not reusable):** `src/game/*`, `src/engine/*` (WDL/acknex startup), MDL/WRS parsers, `entity_render.hpp`, the FMOD mixer, and the hash-pinned Ghidra scripts (`RepairInlineConstants`, `ExportActorPrerequisites`, `LocateFx42Semantics`, …).

**Copyrighted content (never copy):** `Suepercan_Win_TR_Setup/`, `Suepercan_Win_TR_pre-installed/`, all of `analysis/` (Ghidra databases, extracted assets, Wine prefix, DirectX redist).

## 7. Summary reuse table

Paths are relative to each project root. "Relicence" means the copyright holder must grant GPL-3.0-or-later first (§0).

| Need | Best source | Other sources | Verdict | Licence step |
|---|---|---|---|---|
| Signed self-update core | IKV `src/launcher_updates.cpp`, `include/ikv/updates.hpp`, `src/update_helper_main.cpp` | Citroen `updater/src/main.cpp` (Qt); Kabus `installer/update.cpp`, `installer/layout.cpp` | adapt | relicence |
| Manifest signing / release | IKV `tools/sign_manifest.py`, `tools/release.py` | Citroen `packaging/package_native.py`; Kabus `tools/release/*` | adapt | relicence |
| Update security tests | IKV `tests/update_integration.py` | Citroen `updater/tests/safety.py`; Kabus `installer/check_updater_e2e.py` | adapt | relicence |
| Qt launcher | Citroen `launcher/src/main.cpp`, `launcher/src/hardware.cpp` | IKV `src/launcher_main.cpp` (update client), `tests/launcher_gui_smoke.cpp` | `hardware.cpp` as-is; rest adapt | relicence |
| AppImage | Citroen `packaging/appimage/{build,acquire}.py` + `tools.lock.json` | IKV `tools/package_appimage.py` + Dockerfile; Kabus/etiyami uruntime packagers; Pusu `license_bundle.py` | adapt | relicence (Pusu: GPL-3.0-only) |
| CI | Citroen `.github/workflows/native.yml` | Kabus `ci.yml`/`release.yml`; etiyami `release.yml` | adapt | relicence |
| Vulkan function loading | etiyami `native/vulkan_api.hpp` / Citroen `VulkanDispatch.hpp` / Kabus `vulkan_loader.*` | — | reuse as-is (pick one) | relicence |
| SPIR-V embedding | Pusu `cmake/embed_spirv.cmake` (pure CMake) | Kabus `native/embed_spirv.py`; etiyami `tools/embed_spirv.py` | reuse as-is | Pusu GPL-3.0-only or relicence |
| Vulkan pipeline cache | Citroen `VulkanPipelineCache.hpp` | — | reuse (parametrize) | relicence |
| Vulkan RT (ray queries) | IKV `src/vulkan_rt.cpp`, `shaders/vulkan/rt_lighting.glsl` | Kabus `render_vulkan_rt.cpp` + `rt_*.comp`; Citroen `raytracing.cpp`; etiyami `vulkan_effects.cpp` | adapt (scene type) | relicence |
| FSR 3.1.4 Linux | IKV `src/fsr_vk.cpp`, `tools/ffx_fsr3_blobs.py`, `third_party/FidelityFX-SDK` | Kabus, etiyami copies | reuse | FFX MIT; glue relicence |
| Renderer math | IKV `include/ikv/renderer.hpp` (math helpers) + `tests/render_math_test.cpp` | — | reuse | relicence |
| OpenGL renderer base | none generic; all are game-coupled | SuperCan `scene.cpp` window/capture scaffold; Citroen `gl.hpp` proc table | reference | — |
| DXT/BCn decode | Citroen `src/assets/GpuImageBc.cpp` + `src/assets/Images.cpp` (DDS) | IKV `src/assets.cpp` DDS part; SuperCan `src/assets/image.cpp` | adapt; BC4/5 missing everywhere | relicence |
| D3D9 shader → GLSL | SuperCan `src/assets/d3d9_{shader,glsl}.cpp`, `src/assets/effect.cpp` | — | adapt (if TRU shaders are D3D9 bytecode) | relicence |
| SDL3 platform | none generic (IKV/Citroen/etiyami/SuperCan embed SDL3 in game code) | Citroen `src/platform/runtime.cpp` patterns | reference | — |
| GL smoke test (Xvfb) | Citroen `tests/test_native_window.py` | IKV Xvfb recipe (`docs/network/NETWORK_TESTING.md:102`); Pusu `checks/game_cli.py` | adapt | relicence |
| Pixel-assert renderer tests | etiyami `native/check_renderer.cpp`, `native/check_vulkan.cpp` | SuperCan shader self-check | adapt pattern | — |
| Reference capture of original under Wine | Kabus `tools/wine-reference/{wineref,compare}.py` | Citroen `tools/observe_original.py`; Pusu `.work/diagnosis/run_diagnosis.py` | adapt | relicence |
| Ghidra headless driver | Citroen `tools/ghidra/analyze.py` | (OpenUnderworld already has `tools/ghidra/analyze.sh`) | reference | — |
| Ghidra export scripts | SuperCan `ExportVtables.java`, `ExportMachineEvidence.java`, `ExportFloatingEnvironment.java`; Citroen `ExportEvidence.java` | IKV `GraphicsEvidence.java`; Pusu `NativeGameEvidence.java`; Kabus `RecoverKabus.java` | reuse / adapt | relicence |
| PE inventory | IKV `tools/pe_inventory.py` | Citroen `tools/inventory.py`; SuperCan `inspect_containers.py`; Kabus `native/pe_font.hpp` | reuse | relicence |
| Archive extraction (TRU bigfile/DRM) | none — all formats are game-specific | — | not applicable (OpenUnderworld's `src/cdc` is already independent) | — |
| Atomic install / ownership | Pusu `src/importer.cpp` publish(); etiyami `native/install_ownership.hpp` | Kabus `native/saves.cpp` `write_atomic` | adapt | relicence |

## 8. Prior findings applicable to reverse-engineering infrastructure

- **Evidence vocabulary.** Kabus, Citroen and SuperCan `docs/REVERSE_ENGINEERING.md` all use CONFIRMED / HIGH CONFIDENCE / HYPOTHESIS / UNKNOWN, and all state that decompiler output is never authoritative source. OpenUnderworld's "observation / inference / unknown" rule matches.
- **Persistent read-only Ghidra workflow.** Import and analyse once, then use `-process … -noanalysis` (and `-readOnly` where possible) for focused re-exports. Pin the program SHA-256 in each script (Citroen `analyze.py`; Pusu `CubeCensus.java`). Record timeouts as omissions, not as recovered code (Kabus `RecoverKabus.java`).
- **Seeded export.** Seed functions from imports/IAT xrefs and string xrefs, plus one caller layer, with a decompile cap (IKV `ExportEvidence.java`). A breadth-first callee walk to depth N (Pusu `NativeGameEvidence.java`) suits TRU better than "decompile all" **[INFERENCE]**: the existing private export `openunderworld-work/ghidra/exports/functions.tsv` has 18,764 lines.
- **D3D usage discovery.** IKV `GraphicsEvidence.java` finds `IDirect3DDevice9` vtable-offset calls. Directly applicable to TRU's D3D9 renderer once the offsets are verified.
- **RTTI vtables.** SuperCan `ExportVtables.java`; OpenUnderworld already has `tools/ghidra/scripts/ExportRtti.java`.
- **FPU determinism.** Citroen and SuperCan inventory x87/SSE control-word instructions (`FLDCW`, `LDMXCSR`) and build with `-ffp-contract=off`. Worth one export pass on `tru.exe` before porting physics or animation math **[INFERENCE]**.
- **Packed executables.** IKV and Citroen unpack with UPX 5.2.1 into a separate file and keep a separate Ghidra project for it. TRU's GOG `tru.exe` was analysed directly here; packing has not been observed.
- **Differential oracles.**
  - Run the original under Wine in `bwrap --unshare-all` with a private Xvfb/Xwayland and a disposable prefix, and compare captures with SSIM (Kabus, Citroen, Pusu).
  - Call original DLL exports from a Wine-built probe (SuperCan).
  - For TRU, the GOG install already ships dxwrapper/`D3D9.dll`, which matters for reference captures **[INFERENCE]**.
- **Installer format.** Several projects use innoextract 1.9 (signed upstream binary). **[INFERENCE]** GOG offline installers are Inno Setup, so innoextract would work for importing from a GOG installer. OpenUnderworld currently reads an already-installed directory, so this is not needed now.
- **Data hygiene.** Kabus uses an allowlist `.gitignore`. Pusu's tree shows that a protection key landed in source (`resources.cpp:176-211`). OpenUnderworld must keep any key/obfuscation constants out of git unless they are format facts needed for interoperability, and must document them.

## 9. Top recommendations for OpenUnderworld

0. **First, licensing.** Owner confirms authorship and grants GPL-3.0-or-later for each file copied. Otherwise everything below must be re-implemented independently, using these projects as reference only.
1. **Updater.** Base it on **IKV's updater core** (`launcher_updates.cpp` + `update_helper_main.cpp` + `sign_manifest.py` + `update_integration.py`):
   - Ed25519 via OpenSSL `EVP_DigestVerify` over exact manifest bytes, checked before parsing.
   - Monotonic sequence anti-rollback; `renameat2` exchange; journaled `current`/`previous`.
   - Generate a fresh key.
   - Use OpenSSL rather than libsodium (both installed) to match the three existing implementations.
   - Do **not** follow etiyami/Pusu, which trust GitHub digests only. Do not execute downloaded images before verification (Pusu `--appimage-extract`).
   - Keep game data outside the swapped runtime tree; Pusu copies all data on every update.
2. **Launcher.** Qt6 Widgets, structured like **Citroen `launcher/`**:
   - Updater run as a subprocess with JSON events.
   - `hardware.cpp` reused for the Vulkan/RT/GL capability probe.
   - `QT_QPA_PLATFORM=offscreen` smoke test, plus IKV's `findChild` GUI-smoke technique.
3. **Renderer base.** Write OpenUnderworld's own GL 4.6/Vulkan renderer against `src/scene/Assets.hpp`, because every existing renderer is game-coupled. Lift only the small generic pieces:
   - one X-macro Vulkan dispatch (etiyami `vulkan_api.hpp` or Citroen `VulkanDispatch.hpp`);
   - `embed_spirv.cmake`;
   - Citroen `VulkanPipelineCache.hpp`;
   - IKV renderer math helpers + test.
   - Later, adapt IKV `vulkan_rt.cpp` + `rt_lighting.glsl` to OpenUnderworld's `SceneData`.

   All five prior RT implementations use **ray queries** (no SBT). etiyami, IKV and CitroenC4Robot READMEs record RT runs on an RX 7900 XTX/RADV, the same GPU as this workstation. Follow that rather than `VK_KHR_ray_tracing_pipeline` **[INFERENCE: lower complexity]**. FSR 3.1.4 can come from IKV's MIT-vendored port when needed.
4. **Textures.** Upload DXT1/3/5 compressed directly (GL S3TC / `VK_FORMAT_BC*`, as IKV does). Use Citroen `GpuImageBc.cpp` as the CPU fallback. Add BC4/BC5 if TRU's PCD9 formats need them (unverified). No existing project decodes BC4/5/7.
5. **Test harness.**
   - CTest with skip code 77 or `GTEST_SKIP` for game-data tests (OpenUnderworld already uses `GTEST_SKIP` with `OU_GAME_DIR`).
   - A Citroen-style Python smoke test: one owned `Xvfb -displayfd`, never parallel `xvfb-run -a`; `LIBGL_ALWAYS_SOFTWARE=1` for deterministic CI; `MESA_VK_WSI_DEBUG=sw` for Vulkan on Xvfb.
   - PPM capture validated for size and non-uniform content.
   - Pixel-assert synthetic renderer tests in the style of etiyami `check_renderer.cpp`.
6. **Packaging and CI.**
   - Copy the shape of Citroen `.github/workflows/native.yml`: SHA-pinned actions; SDL3 built from a hashed tarball; ctest; on tag, double build + `cmp`, sign, smoke-test the AppImage, draft release.
   - Use Citroen `appimage/acquire.py` + `tools.lock.json` for pinned appimagetool/runtime.
   - Add a Kabus-style allowlist `.gitignore` so no game data can be committed.
7. **RE tooling.** Add SuperCan `ExportVtables.java`, `ExportMachineEvidence.java` and `ExportFloatingEnvironment.java`, IKV `GraphicsEvidence.java` (re-verify the D3D9 vtable offsets) and IKV `pe_inventory.py` alongside the existing `tools/ghidra/scripts`, each with a SHA pin for `tru.exe` (`1fdbd345…1011`).

## Open questions

- Licensing grant for each copied file (§0). This is the owner's decision.
- CitroenC4Robot and SuperCan are not git repositories, and their published state is unknown. Which tree is canonical for Citroen's `native.yml`?
- TRU's shader container format (D3D9 bytecode or something else) decides whether SuperCan's D3D9→GLSL code is relevant. Not yet investigated.
