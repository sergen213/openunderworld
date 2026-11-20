#!/usr/bin/env python3
"""Locate the statically linked IJG libjpeg 6b in tru.exe and tag its functions with evidence.

Method (read-only; objdump of the GOG tru.exe, Ghidra function bounds and direct edges from ExportGraph):
  1. Data anchors: the 124-entry jpeg_std_message_table (first "Bogus message code %d", 0x85CA48), the
     version / copyright strings, the 6b standard quantisation tables (0x85B350 luminance, 0x85B250 chrominance)
     and the float-DCT constants (0x85CC40..).
  2. Named 6b entry points: each one is checked against instruction facts listed in ANCHORS (the script fails if any
     instruction text differs, so the identifications are re-verified on every run).
  3. Region and closure: the code run 0x683DE0..0x695ADF is split into chunks at every Ghidra function entry and at
     every call / pointer target that falls outside a Ghidra body (code Ghidra never made a function: libjpeg is
     reached only through method pointers, so much of it has no Ghidra function). Chunks reached from the named
     entry points through direct calls, jumps and immediate pointer stores get evidence `reach`; the run's
     boundary is checked by listing every reference into it from outside.

Output: docs/re/db/libjpeg.tsv
  entry     chunk start (hex)
  ghidra    1 = a Ghidra function entry, 0 = code with no Ghidra function
  size      chunk bytes (to the next chunk start or the end of the code run)
  name      6b identifier (named entries) or `-`
  role      api | method | util | lib (other libjpeg code) | glue (game C++ code that drives libjpeg) |
            caller (game method that calls the API) | shared (folded stub also called by game code: not libjpeg)
  evidence  named: the checked facts; reach: `reach<-<parent chunk>`; `run` = inside the run, not reached
  ext_refs  references from outside the run (site addresses, ≤ 4)

Usage: tools/re/libjpeg_id.py [--work DIR] [--exe PATH] [--out FILE]
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RUN_LO, RUN_HI = 0x683DE0, 0x695AE0  # libjpeg object run (exclusive end = game wrapper 0x695AE0)
CALLERS_LO, CALLERS_HI = 0x695A10, 0x695DA0  # game glue 0x695A10 + the two screenshot task methods

# entry: (name, role, [(instruction address, substring of objdump text)], note)
ANCHORS = {
    0x684FE0: ("jpeg_std_error", "api", [
        (0x684FE6, "[eax],0x684e70"), (0x684FEC, "[eax+0x4],0x684ed0"), (0x684FF3, "[eax+0x8],0x684e90"),
        (0x684FFA, "[eax+0xc],0x684f10"), (0x685001, "[eax+0x10],0x684fc0"),
        (0x685011, "[eax+0x70],0x85b244"), (0x685018, "[eax+0x74],0x7b")],
        "fills jpeg_error_mgr: 5 methods, jpeg_message_table +0x70, last_jpeg_message +0x74 = 123"),
    0x684E70: ("error_exit", "method", [(0x684FE6, "[eax],0x684e70")], "jpeg_error_mgr slot +0x00"),
    0x684ED0: ("emit_message", "method", [(0x684FEC, "[eax+0x4],0x684ed0")], "slot +0x04"),
    0x684E90: ("output_message", "method", [(0x684FF3, "[eax+0x8],0x684e90")], "slot +0x08"),
    0x684F10: ("format_message", "method", [
        (0x684F1E, "cmp    ecx,DWORD PTR [eax+0x74]"), (0x684F26, "mov    esi,DWORD PTR [edx+ecx*4]"),
        (0x684F2B, "mov    esi,DWORD PTR [eax+0x78]"), (0x684F63, "cmp    cl,0x25")],
        "slot +0x0C: msg_code range check against last_jpeg_message, addon table +0x78, scans for '%'"),
    0x684FC0: ("reset_error_mgr", "method", [(0x685001, "[eax+0x10],0x684fc0")], "slot +0x10"),
    0x683DE0: ("jpeg_CreateCompress", "api", [
        (0x683DEF, "push   0x168"), (0x683DF9, "call   0x814170"), (0x683E07, "call   0x693aa0"),
        (0x683E48, "[esi+0x14],0x64"), (0x695B13, "push   0x168"), (0x695B1C, "push   0x3e")],
        "memset(cinfo,0,0x168 = sizeof jpeg_compress_struct), jinit_memory_mgr, global_state = 100 (CSTATE_START);"
        " caller passes version 62. The version / structsize ERREXITs are absent (see note)"),
    0x68A760: ("jpeg_CreateDecompress", "api", [(0x695C5A, "push   0x1b0"), (0x695C63, "push   0x3e"),
                                               (0x695C6A, "call   0x68a760")],
        "called with version 62, structsize 0x1B0 = sizeof jpeg_decompress_struct"),
    0x693AA0: ("jinit_memory_mgr", "api", [(0x683E07, "call   0x693aa0")], "called by both Create functions"),
    0x683E60: ("jpeg_suppress_tables", "api", [(0x683F9C, "push   0x0"), (0x683F9F, "call   0x683e60")],
        "jpeg_start_compress: if write_all_tables, jpeg_suppress_tables(cinfo, FALSE)"),
    0x683F90: ("jpeg_start_compress", "api", [
        (0x683FA9, "mov    ecx,DWORD PTR [eax+0x10]"), (0x683FB2, "mov    eax,DWORD PTR [edx+0x8]"),
        (0x683FB9, "call   0x686c10"), (0x683FE1, "add    eax,0x65")],
        "err->reset_error_mgr, dest->init_destination, jinit_compress_master, global_state = 101 + raw_data_in"),
    0x686C10: ("jinit_compress_master", "api", [(0x683FB9, "call   0x686c10")], "called by jpeg_start_compress"),
    0x683FF0: ("jpeg_write_scanlines", "api", [(0x695A9F, "call   0x683ff0")],
        "per-row call in the scanline loop 0x695A10 (rows of packed RGB)"),
    0x683ED0: ("jpeg_finish_compress", "api", [(0x695AB7, "call   0x683ed0")], "after the scanline loop"),
    0x688BA0: ("jpeg_set_defaults", "api", [
        (0x688BB2, "push   0x348"), (0x688BC3, "push   0x32"), (0x688BC5, "push   0x85b350"),
        (0x688BCC, "[esi+0x38],0x8")],
        "allocates comp_info (10 x 84 bytes = 0x348), data_precision = 8, quality 75 (scale 50) quant tables"),
    0x6885E0: ("jpeg_set_quality", "api", [
        (0x6885EB, "mov    eax,0x1388"), (0x6885FC, "cmp    ecx,0x64"), (0x688619, "push   0x85b350"),
        (0x688628, "push   0x85b250")],
        "jpeg_quality_scaling inlined (5000 / q, 200 - 2q), jpeg_set_linear_quality inlined"),
    0x688450: ("jpeg_add_quant_table", "api", [(0x688621, "call   0x688450")],
        "called with (cinfo, slot, std table, scale, force)"),
    0x688410: ("jpeg_alloc_quant_table", "api", [(0x688419, "push   0x82"), (0x688426, "[eax+0x80],0x0")],
        "alloc_small 130 bytes (64 UINT16 + sent_table), sent_table = FALSE"),
    0x688430: ("jpeg_alloc_huff_table", "api", [(0x688439, "push   0x112"), (0x688446, "[eax+0x111],0x0")],
        "alloc_small 274 bytes (17 + 256 + sent_table)"),
    0x6883A0: ("jpeg_abort", "api", [(0x6883AC, "[eax+0x24]"), (0x6883BD, "[esi+0x14],0xc8"),
                                     (0x6883D0, "[esi+0x14],0x64")],
        "mem->free_pool(JPOOL_IMAGE); global_state = 200 (decompress) / 100 (compress)"),
    0x6883E0: ("jpeg_destroy", "api", [(0x6883EF, "[eax+0x28]"), (0x6883F8, "[esi+0x14],edi"),
                                       (0x6883FB, "[esi+0x4],edi")],
        "mem->self_destruct; global_state = 0; mem = NULL"),
    0x68A800: ("jpeg_destroy_compress / jpeg_destroy_decompress", "api", [(0x68A800, "jmp    0x6883e0")],
        "identical-code-folded thunk to jpeg_destroy, called by both wrappers"),
    0x68AA70: ("jpeg_read_header", "api", [(0x68AA76, "call   0x68a940"), (0x68AA8B, "call   0x6883a0"),
                                           (0x68AA93, "mov    eax,0x2")],
        "jpeg_consume_input; REACHED_EOI -> jpeg_abort, HEADER_TABLES_ONLY (2)"),
    0x68A940: ("jpeg_consume_input", "api", [(0x68AA76, "call   0x68a940")], "called by jpeg_read_header"),
    0x68ABE0: ("jpeg_start_decompress", "api", [(0x695CA8, "call   0x68abe0")], "after jpeg_read_header(TRUE)"),
    0x68AB80: ("jpeg_read_scanlines", "api", [(0x695D05, "push   0x1"), (0x695D11, "call   0x68ab80")],
        "one row per call in the decode loop"),
    0x68A9F0: ("jpeg_finish_decompress", "api", [(0x695D70, "call   0x68a9f0")], "after the decode loop"),
    0x68EAE0: ("jpeg_resync_to_restart", "api", [(0x68AD76, "[eax+0x14],0x68eae0")],
        "installed as jpeg_source_mgr.resync_to_restart (+0x14) by the game source manager 0x68AD50"),
    0x695950: ("jdiv_round_up", "util", [(0x695958, "lea    eax,[eax+ecx*1-0x1]"), (0x69595D, "idiv   ecx")],
        "(a + b - 1) / b"),
    0x695960: ("jround_up", "util", [(0x695969, "lea    ecx,[eax+esi*1-0x1]"), (0x695975, "sub    eax,edx")],
        "a += b - 1; a - a % b"),
    0x695980: ("jcopy_sample_rows", "util", [(0x695994, "lea    esi,[ecx+eax*4]"), (0x6959B3, "call   0x8148a0")],
        "row-pointer arrays, memcpy per row"),
    0x6959D0: ("jcopy_block_row", "util", [(0x6959DC, "shl    eax,0x7"), (0x6959E2, "call   0x8148a0")],
        "memcpy(num_blocks * 128 = DCTSIZE2 * sizeof(JCOEF))"),
    0x6959F0: ("jzero_far", "util", [(0x6959FC, "call   0x814170")], "memset(target, 0, bytes)"),
    0x690E20: ("jpeg_fdct_float", "lib", [(0x690E24, "fld    QWORD PTR ds:0x85cc58"), (0x690F2A, "0x85cc50"), (0x690F36, "0x85cc48"), (0x690F4C, "0x85cc40")],
        "AA&N float forward DCT constants 0.707106781 (0x85CC58), 0.382683433, 0.541196100, 1.306562965"),
    0x691B40: ("jpeg_idct_float", "lib", [(0x691B44, "0x85cc78"), (0x691B4E, "0x85cc70"), (0x691B58, "0x85cc68"), (0x691D65, "0x85cc60")],
        "AA&N float inverse DCT constants 1.414213562, 1.847759065, 1.082392200, 2.613125930"),
    # Game C++ glue compiled next to libjpeg (thiscall, `ret N`): not IJG code.
    0x68ADE0: ("game memory destination manager setup", "glue", [
        (0x68ADF8, "[eax+0x8],0x68aca0"), (0x68ADFF, "[eax+0xc],0x68ad90"), (0x68AE06, "[eax+0x10],0x68ada0"),
        (0x68AE1A, "ret    0x10")],
        "sets cinfo->dest (+0x18): init / empty_output_buffer / term_destination"),
    0x68AD50: ("game memory source manager setup", "glue", [
        (0x68AD5A, "[ecx+0x18],eax"), (0x68AD68, "[eax+0xc],0x68ac90"), (0x68AD6F, "[eax+0x10],0x68ad30"),
        (0x68AD89, "ret    0xc")],
        "sets cinfo->src (+0x18); fill_input_buffer 0x68AC90 is a folded 3-byte stub shared with game code"),
    0x695AE0: ("Screenshots::CaptureAndSaveTask slot 0", "caller", [
        (0x695B0E, "call   0x684fe0"), (0x695B23, "call   0x683de0"), (0x695B49, "[esp+0x98],0x3"),
        (0x695B54, "[esp+0x9c],0x2"), (0x695B5F, "call   0x688ba0"), (0x695B73, "call   0x6885e0")],
        "vftable 0x85FCD4 slot 0 (rtti.tsv): compresses the screenshot, 3 components, JCS_RGB, to a memory buffer"),
    0x695C00: ("Screenshots::LoadToWidgetsTask slot 2", "caller", [
        (0x695C48, "call   0x814170"), (0x695C55, "call   0x684fe0"), (0x695C6A, "call   0x68a760"),
        (0x695C92, "call   0x68aa70"), (0x695CA8, "call   0x68abe0"), (0x695D70, "call   0x68a9f0")],
        "vftable 0x85FC64 slot 2 (rtti.tsv): decodes a saved screenshot for the load / save widgets"),
    0x695A10: ("game scanline writer", "glue", [(0x695A36, "call   0x683f90"), (0x695A75, "shr    edx,0x10"),
                                               (0x695A9F, "call   0x683ff0"), (0x695AB7, "call   0x683ed0")],
        "thiscall: start_compress, converts 0x00RRGGBB rows to RGB, write_scanlines, finish_compress"),
}

LINE = re.compile(r"^\s*([0-9a-f]+):\t(?:[0-9a-f ]+)\t(.*)$")
IMM = re.compile(r"0x([0-9a-f]{6})\b")


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", type=Path, default=REPO.parent / "openunderworld-work")
    ap.add_argument("--exe", type=Path,
                    default=Path(os.environ.get("TRU_EXE", "/mnt/1st4TB/Tomb Raider Underworld/tru.exe")))
    ap.add_argument("--out", type=Path, default=REPO / "docs/re/db/libjpeg.tsv")
    args = ap.parse_args()

    raw = args.exe.read_bytes()
    # .rdata: VA 0x83E000, raw 0x43E000 (docs/re/EXECUTABLE.md section table).
    def rd(va: int, n: int) -> bytes:
        off = va - 0x83E000 + 0x43E000
        return raw[off:off + n]

    def cstr(va: int) -> str:
        b = rd(va, 256)
        return b[:b.index(b"\0")].decode("latin1")

    # 1. Data anchors.
    table = 0x85CA48
    msgs = []
    while True:
        p = struct.unpack("<I", rd(table + 4 * len(msgs), 4))[0]
        if not 0x83E000 <= p < 0x88E000:
            break
        msgs.append(cstr(p))
    assert len(msgs) == 124 and msgs[0] == "Bogus message code %d", (len(msgs), msgs[:1])
    assert msgs[-1] == "Application transferred too many scanlines"
    assert any(m.startswith("6b  27-Mar-1998") for m in msgs) and any("Thomas G. Lane" in m for m in msgs)
    lum = struct.unpack("<8I", rd(0x85B350, 32))
    chrom = struct.unpack("<8I", rd(0x85B250, 32))
    assert lum == (16, 11, 10, 16, 24, 40, 51, 61) and chrom == (17, 18, 24, 47, 99, 99, 99, 99), (lum, chrom)
    std_err_table = struct.unpack("<8B", rd(0x85B244, 8))

    # 2. Instructions.
    asm = subprocess.run(["objdump", "-d", "-M", "intel", "--start-address=0x401000", "--stop-address=0x83e000",
                          str(args.exe)], check=True, capture_output=True, text=True).stdout
    ins: list[tuple[int, str]] = []
    for line in asm.splitlines():
        m = LINE.match(line)
        if m:
            ins.append((int(m.group(1), 16), m.group(2).strip()))
    text = dict(ins)
    for e, (name, _, checks, _) in ANCHORS.items():
        for a, sub in checks:
            t = text.get(a)
            if t is None or sub not in t:
                sys.exit(f"anchor check failed: {name} {a:#x}: expected '{sub}', got '{t}'")

    fdir = args.work / "re-baseline" / "FunctionDb"
    funcs = {int(r["entry"], 16): r for r in read_tsv(fdir / "functions_full.tsv")}
    bodies = []
    for e, r in funcs.items():
        for rg in r["ranges"].split(";"):
            lo, hi = (int(x, 16) for x in rg.split("-"))
            bodies.append((lo, hi, e))
    bodies.sort()
    blo = [b[0] for b in bodies]

    def in_body(v: int) -> bool:
        i = bisect.bisect_right(blo, v) - 1
        return any(bodies[j][0] <= v <= bodies[j][1] for j in range(max(i - 4, 0), i + 1))

    def refs(t: str) -> list[int]:
        return [int(m.group(1), 16) for m in IMM.finditer(t)]

    # 3. Chunks: Ghidra entries in the run + call targets and stored code pointers inside the run that no Ghidra
    #    body covers (branch targets do not start a chunk: they are inside one).
    starts = {e for e in funcs if RUN_LO <= e < RUN_HI}
    for a, t in ins:
        if t.startswith("j"):
            continue
        for v in refs(t):
            if RUN_LO <= v < RUN_HI and v in text and not in_body(v):
                starts.add(v)
    starts |= {e for e in ANCHORS if RUN_LO <= e < RUN_HI}
    order = sorted(starts)
    chunk_of = lambda v: order[bisect.bisect_right(order, v) - 1]
    glue_out = {e: (e, e + int(funcs[e]["size"])) for e in ANCHORS if not RUN_LO <= e < RUN_HI}
    out_edges = collections.defaultdict(set)
    ext = collections.defaultdict(list)
    for a, t in ins:
        for v in refs(t):
            if not RUN_LO <= v < RUN_HI or v not in text:
                continue
            d = chunk_of(v)
            if RUN_LO <= a < RUN_HI:
                if chunk_of(a) != d:
                    out_edges[chunk_of(a)].add(d)
                continue
            if v != d:
                continue  # an immediate that lands inside a chunk from outside: not a code reference
            ext[d].append(a)
            # Glue outside the run (0x695A10 sits past it) also seeds the closure.
            for g, (lo, hi) in glue_out.items():
                if lo <= a < hi:
                    out_edges[g].add(d)
    via: dict[int, int | None] = {e: None for e in ANCHORS}
    stack = list(ANCHORS)
    while stack:
        c = stack.pop()
        for d in sorted(out_edges.get(c, ())):
            if d not in via:
                via[d] = c
                stack.append(d)

    rows = []
    for i, s in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else RUN_HI
        if s in ANCHORS:
            name, role, checks, note = ANCHORS[s]
            ev = "checked " + ",".join(f"{a:x}" for a, _ in checks) + ": " + note
        else:
            name, role = "-", "lib"
            ev = f"reach<-{via[s]:x}" if s in via else "run"
            foreign = [a for a in ext.get(s, []) if not CALLERS_LO <= a < CALLERS_HI]
            if foreign:
                # Identical-code folding merged it with game functions: not evidence of libjpeg alone.
                role = "shared"
                ev += f"; also called by game code ({len(foreign)} sites)"
        rows.append((s, "1" if s in funcs else "0", end - s, name, role, ev,
                     ";".join(f"{a:x}" for a in sorted(ext.get(s, []))[:4])))
    for e, (name, role, checks, note) in ANCHORS.items():
        if not RUN_LO <= e < RUN_HI:
            ev = "checked " + ",".join(f"{a:x}" for a, _ in checks) + ": " + note
            rows.append((e, "1" if e in funcs else "0", int(funcs[e]["size"]) if e in funcs else 0, name, role, ev,
                         ";".join(f"{a:x}" for a in sorted(ext.get(e, []))[:4])))
    rows.sort()
    with args.out.open("w", encoding="utf-8", newline="") as f:
        f.write("entry\tghidra\tsize\tname\trole\tevidence\text_refs\n")
        for r in rows:
            f.write(f"{r[0]:08x}\t{r[1]}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]}\t{r[6]}\n")

    lib = [r for r in rows if r[4] not in ("glue", "caller", "shared")]
    n_g = sum(1 for r in lib if r[1] == "1")
    reach = sum(1 for r in lib if not r[5].startswith("run"))
    ext_in = sorted({(v, a) for v, sites in ext.items() for a in sites})
    print(f"message table 0x{table:X}: {len(msgs)} entries; jpeg_std_error table pointer 0x85B244 holds "
          f"{std_err_table} in the file")
    print(f"run 0x{RUN_LO:X}..0x{RUN_HI - 1:X}: {len(lib)} libjpeg chunks ({n_g} Ghidra functions, "
          f"{len(lib) - n_g} without), {reach} named or reached, {len(lib) - reach} run-only; "
          f"{sum(1 for r in rows if r[4] in ('glue', 'caller'))} game glue / callers, "
          f"{sum(1 for r in rows if r[4] == 'shared')} shared")
    print("references into the run from outside:", ", ".join(f"{v:x}<-{a:x}" for v, a in ext_in))
    return 0


if __name__ == "__main__":
    sys.exit(main())
