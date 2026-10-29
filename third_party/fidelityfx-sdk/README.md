# AMD FSR SDK 2.4.0 subset (ffx-api, FSR 3.1.6 upscaler + frame generation, Vulkan)

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK tag `v2.4.0`, commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`
  (`Kits/FidelityFX/`: `api/`, `backend/vk/`, `upscalers/`, `framegeneration/{include,internal,fsr3/include,fsr3/internal}`).
- Licence: MIT (`LICENSE.txt`; the SDK's `docs/license.md` lists every vendored file under its MIT section). Installed as
  `share/licenses/openunderworld/fidelityfx-sdk/` in the AppImage.
- Not vendored and not usable on Linux: `signedbin/*.dll` (FSR 4.1.2 ML upscaler, FSR 4.0.x frame generation, the ffx
  loader) are signed Windows PE DLLs under a binary-only, no-reverse-engineering licence; the Vulkan frame-generation
  swapchain (`framegeneration/fsr3/vk`) is Win32-bound and replaced by our own presentation (VkRenderer::present).

The release ships no build files, no shader permutation headers and not AMD's `FidelityFX_SC` generator, and its shaders
are HLSL only. `CMakeLists.txt` builds `ou_ffx` (ffx-api with the providers linked statically); `tools/ffx_shaders.py`
compiles the HLSL passes with DXC (`-spirv`, pinned DirectXShaderCompiler v1.9.2609 release download unless `dxc` is
installed) and writes the `*_permutations.h` tables with SPIR-V reflection. Only the permutations our renderer can
request are compiled (render-resolution unjittered motion vectors, standard depth; LDR/HDR, LUT and sharpening
variants); wave64 / FP16 tables alias the wave-agnostic FP32 blobs.

Local glue (`ou/`): `ffx_compat.h` (force-included: MSVC CRT shims, `__declspec`, the permutation-table entry type,
larger opaque contexts for 4-byte `wchar_t`), `ffx_providers.cpp` (provider registry), `ffx_provider_external.h`
(stand-in for the driver-provider bridge, absent from the release), `miniz.h` (stand-in; our blobs are uncompressed),
`git_hash_branch.h` (absent generated header).

Local patches (marked `ou patch N`):
1. `backend/vk/ffx_vk.cpp` `EffectContext`: no `alignas(32)` (carved from scratch memory at 8-byte offsets).
2. `backend/vk/ffx_vk.cpp` `findMemoryTypeIndex`: skip AMD device-coherent memory types unless requested (RADV).
3. `api/internal/ffx_provider.h`: includes `ou/ffx_provider_external.h`.
4. `ffx_provider_fsr3upscale.cpp`, `ffx_provider_fsr3framegeneration.cpp`: MSVC `ui64` literal suffix -> `ull`.
5. `api/internal/ffx_internal_types.h`: `FFX_SDK_DEFAULT_CONTEXT_SIZE` overridable.
6. `api/internal/ffx_internal_types.h`: `FFX_MAX_QUEUED_FRAMES` 4 -> 8 (frame generation's per-frame graph executions
   recycled image views of frames still in flight).

Upgrading: replace the upstream directories, re-apply the patches, rerun the build; the renderer only talks ffx-api
(`src/render/vk/VkFsr.cpp`).
