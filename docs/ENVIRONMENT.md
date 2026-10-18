# Development environment (detected 2026-10-18)

| Item | Value |
|---|---|
| OS | CachyOS Linux (Arch-based, `ID_LIKE=arch`) |
| Kernel | 7.2.9-2-cachyos x86_64 |
| Desktop | KDE Plasma 6.8.0, Wayland session |
| CPU | AMD Ryzen 9 9950X3D, 32 threads |
| RAM | 30 GiB |
| GPU | AMD Radeon RX 7900 XTX (Navi 31) |
| GL driver | Mesa 26.2.4 radeonsi, OpenGL 4.6 core / compat, GLSL 4.60 |
| Vulkan driver | Mesa 26.2.4 RADV, API 1.4.354, `VK_KHR_ray_tracing_pipeline` exposed |
| Storage | `/mnt/1st4TB` = `/dev/sda1`, 1.2 TB free |

## Toolchain (installed via pacman)

cmake 4.4.4, ninja 1.13.2, gcc 16.2.1, clang 23.1.3, git 2.56, github-cli 2.102 (authenticated as `sergen213`),
python 3.14.8, JDK 27 (default `java-27-openjdk`), ghidra 12.1.2 (`/opt/ghidra`), wine 11.20 (reference only, never the final runtime), ffmpeg 9.0.2.

Libraries: sdl3 3.4.18, qt6-base/tools/declarative 6.12.0, vulkan-headers/icd-loader 1.4.363, shaderc (glslc), glslang, spirv-tools,
glm 1.0.3, zlib-ng-compat 2.3.3, zstd, lz4, openssl 3.6.5, libsodium 1.0.22, gtest 1.18, nlohmann-json 3.12, libepoxy 1.5.10, glew 2.3.1.

## Missing, proposed (not installed — awaiting approval)

```sh
sudo pacman -S --needed vulkan-validation-layers volk stb minisign
```

| Package | Needed for | Blocks now? |
|---|---|---|
| vulkan-validation-layers | Vulkan backend debugging (milestone 6) | No |
| volk | Vulkan function loading (optional; can load via `vkGetInstanceProcAddr`) | No |
| stb | PNG dumps / image IO in tools (TGA/PPM used meanwhile) | No |
| minisign | Manual signing of release manifests (libsodium covers verification in-app) | No |

Not available in repos: imgui, vulkan-memory-allocator (diagnostics overlay is self-written; VMA can be vendored later if needed).

## Original game

GOG build, `/mnt/1st4TB/Tomb Raider Underworld` (read-only to all project tools). gameId 1900745791, buildId 58082205556264096,
`tru.exe` sha256 `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011`. Third-party files present in the install:
`dxwrapper.dll/.ini`, `D3D9.dll`, `DEPHook.asi` — ignored by the native port.

## Layout

- Repo: `/mnt/1st4TB/Projects/openunderworld` (public-safe content only).
- Private work: `/mnt/1st4TB/Projects/openunderworld-work` (Ghidra project/exports, research clones, manifests, extracted data).
