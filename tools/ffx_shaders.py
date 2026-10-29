#!/usr/bin/env python3
"""Build the FidelityFX SDK 2.4.0 Vulkan shader permutation headers (FSR 3.1.6 upscaler, frame interpolation,
optical flow) with DXC, replacing AMD's FidelityFX_SC tool, which the SDK does not ship.

For every `<pass>[_wave64][_16bit]_permutations.h` the SDK's *_shaderblobs.cpp includes, this compiles the pass's
HLSL to SPIR-V for each option combination in OPTIONS, reflects its descriptor bindings and writes the header the
blob accessor expects: `<pass>_PermutationKey` (one bit per option, in the accessor's POPULATE_PERMUTATION_KEY
order), `g_<name>_IndirectionTable` (key index -> PermutationInfo index) and `g_<name>_PermutationInfo`.
Option combinations that are not compiled map to a last, empty entry: creating that pipeline fails (vkCreateShaderModule
with no code), so an unexpected request errors instead of running the wrong shader.

The wave64 and 16-bit variants alias the wave-agnostic FP32 blobs (ponytail: correct on every device; compile real
FFX_HALF / [WaveSize(64)] variants if profiles ever show the FP32 passes as a cost).
"""

import argparse
import concurrent.futures
import itertools
import re
import struct
import subprocess
from pathlib import Path

# Per accessor: options compiled (others are fixed by docs/RENDERING.md "FSR": render-resolution, unjittered
# motion vectors, standard depth).
OPTIONS = {
    "upscalers/fsr3/internal/ffx_fsr3upscaler_shaderblobs.cpp": {
        "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE": (0, 1),
        "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT": (0, 1),
        "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": (1,),
        "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": (0,),
        "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": (0,),
        "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING": (0, 1),
    },
    "framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.cpp": {
        "FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS": (1,),
        "FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS": (0,),
        "FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH": (0,),
    },
    "framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.cpp": {
        "FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT": (0, 1),
    },
}
INCLUDES = ("api/internal/gpu", "upscalers/fsr3/include/gpu", "framegeneration/fsr3/include/gpu")
# One Vulkan descriptor set: SRVs at t, UAVs at 100 + u, constant buffers at 200 + b, samplers at 320 + s
# (ffx_vk.cpp SAMPLER_BINDING_SHIFT binds the effect's static samplers there).
DXC_FLAGS = ("-spirv", "-fspv-target-env=vulkan1.2", "-T", "cs_6_2", "-E", "CS", "-HV", "2021", "-O3",
             "-fvk-use-dx-layout", "-fvk-u-shift", "100", "0", "-fvk-b-shift", "200", "0", "-fvk-s-shift", "320", "0",
             "-DFFX_GPU=1", "-DFFX_HLSL=1", "-DFFX_HALF=0")
VARIANTS = ("", "_wave64", "_16bit", "_wave64_16bit")
CATEGORIES = ("constantBuffer", "srvTexture", "uavTexture", "srvBuffer", "uavBuffer", "sampler", "rtAccelerationStructure")

# SPIR-V enumerants (reflection after tools of the same name in the SDK's FidelityFX_SC).
OP_NAME, OP_DECORATE, OP_MEMBER_DECORATE = 5, 71, 72
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 25, 26, 28, 29, 30, 32
OP_CONSTANT, OP_VARIABLE, OP_TYPE_ACCELERATION_STRUCTURE = 43, 59, 5341
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12


def reflect(spv: bytes, what: str) -> dict:
    words = struct.unpack(f"<{len(spv) // 4}I", spv)
    assert words[0] == 0x07230203, what
    names, decorations, non_writable, types, constants, variables = {}, {}, set(), {}, {}, []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        o = words[i + 1:i + count]
        if op == OP_NAME:
            raw = b"".join(struct.pack("<I", w) for w in o[1:])
            names[o[0]] = raw[:raw.index(b"\0")].decode()
        elif op == OP_DECORATE:
            decorations.setdefault(o[0], {})[o[1]] = o[2:]
        elif op == OP_MEMBER_DECORATE and o[2] == DEC_NON_WRITABLE:
            non_writable.add(o[0])
        elif op in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER,
                    OP_TYPE_ACCELERATION_STRUCTURE):
            types[o[0]] = (op, o[1:])
        elif op == OP_CONSTANT:
            constants[o[1]] = o[2]
        elif op == OP_VARIABLE:
            variables.append((o[1], o[0], o[2]))
        i += count
    found = []
    for var, pointer, storage in variables:
        dec = decorations.get(var, {})
        if storage not in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER) or DEC_BINDING not in dec:
            continue
        name = names.get(var, "")
        tid, n = types[pointer][1][1], 1
        while types.get(tid, (None,))[0] in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            op, ops = types[tid]
            n = 0 if op == OP_TYPE_RUNTIME_ARRAY else n * constants[ops[1]]
            tid = ops[0]
        op, ops = types[tid]
        if op == OP_TYPE_IMAGE:
            kind = "srvTexture" if ops[5] == 1 else "uavTexture"
        elif op == OP_TYPE_SAMPLER:
            kind = "sampler"
        elif op == OP_TYPE_ACCELERATION_STRUCTURE:
            kind = "rtAccelerationStructure"
        elif op == OP_TYPE_STRUCT and (storage == SC_STORAGE_BUFFER or DEC_BUFFER_BLOCK in decorations.get(tid, {})):
            kind = "srvBuffer" if tid in non_writable else "uavBuffer"
        elif op == OP_TYPE_STRUCT:
            kind = "constantBuffer"
        else:
            raise SystemExit(f"{what}: unsupported resource {name}")
        found.append((dec.get(DEC_SET, (0,))[0], dec[DEC_BINDING][0], kind, name, n))
    out = {c: [] for c in CATEGORIES}
    for space, binding, kind, name, n in sorted(found):
        out[kind].append((name, binding, n, space))
    return out


def accessor_passes(cpp: Path) -> tuple[list[str], list[str]]:
    text = cpp.read_text()
    passes = sorted({re.sub(r"(_wave64)?(_16bit)?$", "", h) for h in re.findall(r"<([a-z0-9_]+)_permutations\.h>", text)})
    macro = text[text.index("#define POPULATE_PERMUTATION_KEY"):]
    keys = re.findall(r"key\.(FFX_[A-Z0-9_]+)\s*=", macro[:macro.index("\n\n")])
    return passes, keys


def compile_pass(dxc: str, sdk: Path, source: Path, defines: dict) -> bytes:
    cmd = [dxc, *DXC_FLAGS, *(f"-D{k}={v}" for k, v in defines.items()), *(f"-I{sdk / i}" for i in INCLUDES),
           "-Fo", "/dev/stdout", str(source)]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode or not r.stdout:
        raise SystemExit(f"dxc failed for {source.name} {defines}:\n{r.stderr.decode()}")
    return r.stdout


def array(ctype: str, name: str, values: list[str]) -> str:
    return f"static const {ctype} {name}[] = {{ {', '.join(values) if values else '0'} }};\n"


def header(pass_name: str, variant: str, keys: list[str], perms: list[tuple[int, bytes, dict]]) -> str:
    sym = f"g_{pass_name}{variant}"
    out = [f"// Generated by tools/ffx_shaders.py from {pass_name}.hlsl. Do not edit.\n#pragma once\n#include <stdint.h>\n"]
    guard = f"OU_FFX_KEY_{pass_name.upper()}"
    out.append(f"#ifndef {guard}\n#define {guard}\ntypedef union {pass_name}_PermutationKey {{\n    struct {{\n")
    out += [f"        uint32_t {k} : 1;\n" for k in keys]
    out.append(f"    }};\n    uint32_t index;\n}} {pass_name}_PermutationKey;\n#endif\n")
    if not variant:
        for idx, (_, spv, refl) in enumerate(perms):
            out.append(array("uint8_t", f"{sym}_blob{idx}", [str(b) for b in spv]))
            for cat in CATEGORIES:
                res = refl[cat]
                out.append(array("char*", f"{sym}_{cat}Names{idx}", [f'(char*)"{r[0]}"' for r in res]))
                for field, col in (("Bindings", 1), ("Counts", 2), ("Spaces", 3)):
                    out.append(array("uint32_t", f"{sym}_{cat}{field}{idx}", [str(r[col]) for r in res]))
    base = f"g_{pass_name}"
    table = ["-1"] * (1 << len(keys))
    for idx, (key, _, _) in enumerate(perms):
        table[key] = str(idx)
    table = [t if t != "-1" else str(len(perms)) for t in table]  # empty sentinel entry
    out.append(array("int32_t", f"{sym}_IndirectionTable", table))
    entries = []
    for idx, (_, spv, refl) in enumerate(perms):
        fields = [f"{base}_blob{idx}", f"{len(spv)}u", '"CS"'] + [f"{len(refl[c])}u" for c in CATEGORIES]
        for cat in CATEGORIES:
            fields += [f"(const char**){base}_{cat}Names{idx}"] + [f"{base}_{cat}{f}{idx}" for f in ("Bindings", "Counts", "Spaces")]
        entries.append("    { " + ", ".join(fields) + " }")
    entries.append("    { nullptr, 0u, \"CS\"" + ", 0u" * len(CATEGORIES) + ", nullptr" * (4 * len(CATEGORIES)) + " }")
    out.append(f"static const OuFfxPermutationInfo {sym}_PermutationInfo[] = {{\n" + ",\n".join(entries) + "\n};\n")
    return "".join(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dxc", required=True)
    ap.add_argument("--sdk", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    jobs = []
    for cpp, options in OPTIONS.items():
        passes, keys = accessor_passes(args.sdk / cpp)
        assert sorted(keys) == sorted(options), (cpp, keys)
        shaders = (args.sdk / cpp).parent / "shaders"
        for p in passes:
            for values in itertools.product(*(options[k] for k in keys)):
                defines = dict(zip(keys, values))
                index = sum(v << bit for bit, v in enumerate(values))
                jobs.append((p, keys, index, shaders / f"{p}.hlsl", defines))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        spvs = list(pool.map(lambda j: compile_pass(args.dxc, args.sdk, j[3], j[4]), jobs))
    per_pass = {}
    for (p, keys, index, src, _), spv in zip(jobs, spvs):
        per_pass.setdefault(p, (keys, []))[1].append((index, spv, reflect(spv, src.name)))
    for p, (keys, perms) in per_pass.items():
        for v in VARIANTS:
            text = header(p, v, keys, perms)
            if v:  # variants alias the FP32 wave-agnostic blobs of the base header
                text = text.replace("#pragma once\n", f"#pragma once\n#include \"{p}_permutations.h\"\n", 1)
            target = args.output / f"{p}{v}_permutations.h"
            if not target.exists() or target.read_text() != text:
                target.write_text(text)
    print(f"FidelityFX shaders: {len(jobs)} SPIR-V permutations, {len(per_pass)} passes")


if __name__ == "__main__":
    main()
