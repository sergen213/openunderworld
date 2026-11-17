#!/usr/bin/env python3
"""Build the tru.exe class, structure and global-variable inventory.

Inputs (all read-only):
  <game>/tru.exe                                     RTTI parsed straight from the PE (pefile), .text disassembled
                                                     with `objdump -d -M intel` (binutils, i386)
  <work>/re-baseline/FunctionDb/functions_full.tsv   Ghidra function bodies (tools/ghidra/scripts/ExportGraph.java)
  <work>/re-baseline/FunctionDb/data_symbols.tsv     Ghidra data symbols (user-defined names)
  <work>/ghidra/exports/{rtti,imports,strings}.tsv   analyze.sh exports (validation, import/string kinds)
  docs/re/db/{functions,rtti,citations}.tsv          tools/re/function_db.py outputs (subsystems, cited slots)
  tools/ghidra/names/tru_gog.csv                     applied names
  git ls-files of src/ tests/ docs/ tools/ CHANGELOG.md   repo-established structure offsets

Outputs (docs/re/db/, deterministic, no decompiled code):
  classes.tsv          one row per RTTI type descriptor: vftables, slots, bases, family, sizeof evidence, ctors/dtors
  class_hierarchy.tsv  one row per (class, base) from each ClassHierarchyDescriptor's BaseClassArray
  structures.tsv       one row per (structure, offset) the repo establishes, plus instruction-level checks
  globals.tsv          one row per .rdata/.data/.bss address referenced by an instruction inside a function
  structures_summary.json  counts quoted in docs/re/STRUCTURES.md

Usage: tools/re/struct_db.py [--exe PATH] [--work DIR] [--out DIR]
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pefile

REPO = Path(__file__).resolve().parents[2]
TEXT_LO, TEXT_HI = 0x401000, 0x83DFFF
DATA_LO, DATA_HI = 0x83E000, 0x10D4FFF  # .rdata + .data (incl. .bss tail)
SCAN_ROOTS = ("src", "tests", "docs", "tools", "CHANGELOG.md")
SKIP_PATHS = ("docs/re/db/", "tools/re/struct_db.py", "docs/re/STRUCTURES.md", "tools/ghidra/names/",
              "docs/re/BINARIES.md", "tools/re/pe_inventory.py", "docs/re/FUNCTION_DATABASE.md",
              "tools/re/function_db.py", "third_party/")
TEXT_EXT = {".cpp", ".hpp", ".h", ".c", ".glsl", ".vert", ".frag", ".comp", ".py", ".sh", ".java", ".md", ".plan",
            ".json", ".txt"}


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def write_tsv(path: Path, header: list[str], rows: list[list]) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            f.write("\t".join(str(x).replace("\t", " ").replace("\n", " ") for x in r) + "\n")


def hx(v: int) -> str:
    return f"{v:08x}"


# ---------------------------------------------------------------------------------------------------------------
# PE image + RTTI
# ---------------------------------------------------------------------------------------------------------------

class PE:
    def __init__(self, path: Path):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            lo = self.base + s.VirtualAddress
            self.secs.append((name, lo, lo + max(s.Misc_VirtualSize, s.SizeOfRawData), lo + s.SizeOfRawData,
                              s.get_data()))

    def sec(self, va: int):
        for s in self.secs:
            if s[1] <= va < s[2]:
                return s
        return None

    def section_name(self, va: int) -> str:
        s = self.sec(va)
        if not s:
            return ""
        return s[0] + ("(bss)" if va >= s[3] else "")

    def u32(self, va: int):
        s = self.sec(va)
        if not s or va + 4 > s[3]:
            return None
        o = va - s[1]
        return int.from_bytes(s[4][o:o + 4], "little")

    def i32(self, va: int):
        v = self.u32(va)
        return None if v is None else (v - (1 << 32) if v & 0x80000000 else v)

    def cstr(self, va: int, limit: int = 4096):
        s = self.sec(va)
        if not s:
            return None
        o = va - s[1]
        e = s[4].find(b"\0", o, o + limit)
        if e < 0:
            return None
        try:
            return s[4][o:e].decode("ascii")
        except UnicodeDecodeError:
            return None

    def in_text(self, v) -> bool:
        return v is not None and TEXT_LO <= v <= TEXT_HI


def demangle_all(mangled: list[str]) -> dict[str, str]:
    """`.?AVFoo@bar@@` -> `bar::Foo` via llvm-undname on the matching vftable symbol; simple fallback."""
    out = {}
    syms = ["??_7" + m[4:] + "6B@" for m in mangled]
    try:
        r = subprocess.run(["llvm-undname"], input="\n".join(syms) + "\n", capture_output=True, text=True, check=True)
        lines = [l for l in r.stdout.split("\n")]
        # llvm-undname echoes each input line, then the result, then a blank line.
        res = []
        i = 0
        while i < len(lines):
            if lines[i].startswith("??_7"):
                res.append(lines[i + 1] if i + 1 < len(lines) else "")
                i += 2
            else:
                i += 1
        if len(res) != len(syms):
            raise RuntimeError("undname count mismatch")
    except (OSError, subprocess.CalledProcessError, RuntimeError):
        res = [""] * len(syms)
    for m, d in zip(mangled, res):
        d = d.strip()
        if d.startswith("const ") and d.endswith("::`vftable'"):
            out[m] = d[6:-11]
        else:
            parts = m[4:].rstrip("@").split("@")
            out[m] = "::".join(reversed([p for p in parts if p]))
    return out


class Rtti:
    """Type descriptors, complete object locators, hierarchy descriptors and vftables found by scanning the PE."""

    def __init__(self, pe: PE):
        self.pe = pe
        rd = next(s for s in pe.secs if s[0] == ".rdata")
        dt = next(s for s in pe.secs if s[0] == ".data")
        # Type descriptors: {type_info vftable, spare 0, ".?A..." name}. The type_info vftable is the most common
        # pointer preceding a ".?AV" name.
        self.td = {}
        cand = collections.Counter()
        raw = dt[4]
        for m in re.finditer(rb"\.\?A[VUW]", raw):
            o = m.start()
            if o % 4 == 0 and o >= 8:
                cand[int.from_bytes(raw[o - 8:o - 4], "little")] += 1
        self.type_info_vft = cand.most_common(1)[0][0]
        for m in re.finditer(rb"\.\?A[VUW]", raw):
            o = m.start()
            if o % 4 or o < 8 or int.from_bytes(raw[o - 8:o - 4], "little") != self.type_info_vft:
                continue
            va = dt[1] + o - 8
            name = pe.cstr(va + 8)
            if name:
                self.td[va] = name
        # Complete object locators in .rdata: {sig 0, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor}.
        self.col = {}
        data = rd[4]
        words = {}
        for o in range(0, len(data) - 3, 4):
            words[rd[1] + o] = int.from_bytes(data[o:o + 4], "little")
        self.words = words
        for va, w in words.items():
            if w != 0:
                continue
            td = words.get(va + 12)
            chd = words.get(va + 16)
            if td in self.td and chd in words and words.get(chd) == 0:
                self.col[va] = {"offset": words[va + 4], "cd": words[va + 8], "td": td, "chd": chd}
        # vftables: the dword before slot 0 points at a COL.
        self.vft = {}
        colrefs = {}
        for va, w in words.items():
            if w in self.col and pe.in_text(words.get(va + 4)):
                colrefs[va] = w
        for ref, colva in sorted(colrefs.items()):
            vt = ref + 4
            slots = []
            a = vt
            while pe.in_text(words.get(a)) and (a == vt or a not in colrefs):
                slots.append(words[a])
                a += 4
            self.vft[vt] = {"col": colva, "slots": slots, **self.col[colva]}
        # Hierarchy descriptors.
        self.chd = {}
        for c in self.col.values():
            chd = c["chd"]
            if chd in self.chd:
                continue
            attr, n, bca = words[chd + 4], words[chd + 8], words[chd + 12]
            bases = []
            for i in range(n):
                bcd = words.get(bca + 4 * i)
                if bcd is None or bcd not in words:
                    break
                bases.append({"td": words[bcd], "contained": words[bcd + 4], "mdisp": pe.i32(bcd + 8),
                              "pdisp": pe.i32(bcd + 12), "vdisp": pe.i32(bcd + 16), "attr": words[bcd + 20]})
            self.chd[chd] = {"attr": attr, "bases": bases, "va": chd}
        mangled = sorted(set(self.td.values()))
        self.names = demangle_all(mangled)

    def name(self, td: int) -> str:
        return self.names.get(self.td.get(td, ""), hx(td))


def hierarchy_tree(bases: list[dict]) -> list[tuple[int, int, int]]:
    """(index, parent index, depth) per BaseClassArray entry; entry 0 is the class itself. Each entry's
    numContainedBases is the size of its subtree, which follows it in pre-order."""
    res = []
    end = []  # (index, end index exclusive)
    for i, b in enumerate(bases):
        while end and i >= end[-1][1]:
            end.pop()
        parent = end[-1][0] if end else -1
        res.append((i, parent, len(end)))
        end.append((i, i + 1 + b["contained"]))
    return res


# ---------------------------------------------------------------------------------------------------------------
# Disassembly
# ---------------------------------------------------------------------------------------------------------------

RE_LINE = re.compile(r"^\s*([0-9a-f]+):\t(\S+)\s*(.*)$")
RE_HEXTOK = re.compile(r"0x([0-9a-f]+)")
WIDTH = {"BYTE": 1, "WORD": 2, "DWORD": 4, "QWORD": 8, "TBYTE": 10, "XMMWORD": 16, "FWORD": 6}
READ_ONLY_DST = {"cmp", "test", "push", "call", "jmp", "fld", "fild", "fcom", "fcomp", "fadd", "fsub", "fmul", "fdiv",
                 "fsubr", "fdivr", "ficom", "fiadd", "fimul", "fidiv", "fisub", "comiss", "ucomiss", "bt", "fldcw",
                 "fcomip", "fucomip", "fucom", "fucomp", "fld1", "prefetcht0", "prefetchnta", "fidivr", "fisubr",
                 "comisd", "ucomisd"}
RMW = {"add", "sub", "and", "or", "xor", "inc", "dec", "neg", "not", "shl", "shr", "sar", "sal", "rol", "ror", "adc",
       "sbb", "xchg", "xadd", "cmpxchg", "bts", "btr", "btc"}


def split_ops(ops: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in ops:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur:
        out.append(cur)
    return [o.strip() for o in out]


class Disasm:
    def __init__(self, exe: Path, funcs_path: Path):
        r = subprocess.run(["objdump", "-d", "-M", "intel", "--no-show-raw-insn", "-j", ".text", str(exe)],
                           capture_output=True, text=True, check=True)
        self.addr, self.mn, self.ops = [], [], []
        for line in r.stdout.split("\n"):
            m = RE_LINE.match(line)
            if not m:
                continue
            mn = m.group(2)
            ops = m.group(3)
            if mn in ("rep", "repz", "repnz", "lock") and ops:
                p = ops.split(None, 1)
                mn = mn + " " + p[0]
                ops = p[1] if len(p) > 1 else ""
            self.addr.append(int(m.group(1), 16))
            self.mn.append(mn)
            self.ops.append(ops)
        self.idx = {a: i for i, a in enumerate(self.addr)}
        # Function bodies (Ghidra ranges): instruction -> entry.
        self.func_of = [None] * len(self.addr)
        self.funcs = {}
        ranges = []
        for row in read_tsv(funcs_path):
            e = int(row["entry"], 16)
            self.funcs[e] = row
            for rg in row["ranges"].split(";"):
                lo, hi = (int(x, 16) for x in rg.split("-"))
                ranges.append((lo, hi, e))
        ranges.sort()
        for lo, hi, e in ranges:
            i = bisect.bisect_left(self.addr, lo)
            while i < len(self.addr) and self.addr[i] <= hi:
                if self.func_of[i] is None:
                    self.func_of[i] = e
                i += 1
        self.body = collections.defaultdict(list)
        for i, e in enumerate(self.func_of):
            if e is not None:
                self.body[e].append(i)

    def text(self, i: int) -> str:
        return f"{self.addr[i]:x}: {self.mn[i]} {self.ops[i]}".strip()


def call_target(ops: str):
    m = re.fullmatch(r"0x([0-9a-f]+)", ops)
    return int(m.group(1), 16) if m else None


RE_MEM = re.compile(r"(?:(BYTE|WORD|DWORD|QWORD|TBYTE|XMMWORD|FWORD) PTR )?(?:[a-z]s:)?\[([^\]]+)\]")
RE_REGOFF = re.compile(r"^(e[a-z]{2})(?:\+0x([0-9a-f]+)|-0x([0-9a-f]+))?$")


def mem_operand(op: str):
    """(width, base reg or None, displacement, has index) for an Intel memory operand, else None."""
    m = RE_MEM.search(op)
    if m:
        width = WIDTH.get(m.group(1) or "", 0)
        inner = m.group(2)
        rm = RE_REGOFF.match(inner)
        if rm:
            d = int(rm.group(2) or "0", 16) - int(rm.group(3) or "0", 16)
            return width, rm.group(1), d, False
        disp = 0
        for t in RE_HEXTOK.findall(inner):
            disp = int(t, 16)
        return width, None, disp, ("*" in inner or "+" in inner)
    m = re.match(r"(?:(BYTE|WORD|DWORD|QWORD|TBYTE|XMMWORD|FWORD) PTR )?ds:0x([0-9a-f]+)$", op)
    if m:
        return WIDTH.get(m.group(1) or "", 0), None, int(m.group(2), 16), False
    return None


# ---------------------------------------------------------------------------------------------------------------
# Constructors, destructors, allocation sizes
# ---------------------------------------------------------------------------------------------------------------

RE_VFT_STORE = re.compile(r"^DWORD PTR \[(e[a-z]{2})(?:\+0x([0-9a-f]+))?\],0x([0-9a-f]+)$")


def analyse_objects(dis: Disasm, rtti: Rtti):
    vfts = rtti.vft
    stores = collections.defaultdict(list)  # function -> [(instr index, reg, offset, vftable)]
    for i, mn in enumerate(dis.mn):
        if mn != "mov" or dis.func_of[i] is None:
            continue
        m = RE_VFT_STORE.match(dis.ops[i])
        if not m:
            continue
        vt = int(m.group(3), 16)
        if vt in vfts:
            stores[dis.func_of[i]].append((i, m.group(1), int(m.group(2) or "0", 16), vt))
    slot_funcs = collections.defaultdict(set)  # function -> vftables holding it
    for vt, v in vfts.items():
        for s in v["slots"]:
            slot_funcs[s].add(vt)

    def calls_of(e):
        return [(i, call_target(dis.ops[i])) for i in dis.body.get(e, []) if dis.mn[i] == "call"]

    # Scalar/vector deleting destructors: a vftable slot that tests bit 0 of its stack argument and returns 4 bytes.
    sdtor = {}
    for f in slot_funcs:
        body = dis.body.get(f, [])
        if not body or len(body) > 80:
            continue
        has_test = any(dis.mn[i] == "test" and re.match(r"BYTE PTR \[esp\+0x[48c]\],0x[13]$", dis.ops[i])
                       for i in body)
        has_test = has_test or any(dis.mn[i] == "mov" and re.match(r"[a-d]l,BYTE PTR \[esp\+0x[48c]\]$", dis.ops[i])
                                   for i in body)
        rets = [dis.ops[i] for i in body if dis.mn[i] == "ret"]
        if has_test and rets and all(r == "0x4" for r in rets):
            sdtor[f] = True
    # Deallocator: most common callee of the scalar deleting destructors among calls taking the object.
    dealloc_count = collections.Counter()
    for f in sdtor:
        cs = [c for _, c in calls_of(f) if c is not None]
        if cs:
            dealloc_count[cs[-1]] += 1
    dtor = {}
    for f in sdtor:
        for i, c in calls_of(f):
            if c in stores and c not in sdtor:
                dtor[c] = f
    # Walk each vftable-storing function from its entry up to the first `ret`, tracking registers that hold the
    # incoming `this` (ecx). Its class is the last offset-0 store through `this` (a derived ctor stores its own
    # vftable after the base ctor's); destructors reset the most-derived one first. Field writes through `this`
    # give a lower bound on sizeof for constructors.
    func_class = {}
    ctor_extent = {}
    for f in stores:
        aliases = {"ecx"}
        hi = 0
        zero = []
        for i in dis.body.get(f, []):
            mn, ops = dis.mn[i], dis.ops[i]
            if mn == "ret":
                break
            if mn == "call":
                aliases &= {"esi", "edi", "ebx", "ebp"}
                continue
            ops_l = split_ops(ops)
            if mn == "mov":
                m = RE_VFT_STORE.match(ops)
                if m and m.group(1) in aliases and not m.group(2) and vfts.get(int(m.group(3), 16), {}).get(
                        "offset") == 0:
                    zero.append(int(m.group(3), 16))
            if ops_l and mn not in READ_ONLY_DST:
                mo = mem_operand(ops_l[0])
                if mo and mo[1] in aliases and 0 <= mo[2] < 0x10000:
                    hi = max(hi, mo[2] + (mo[0] or 4))
            if mn in ("mov", "lea", "xor", "pop", "add", "sub", "and", "or") and ops_l \
                    and re.fullmatch(r"e[a-z]{2}", ops_l[0]):
                d = ops_l[0]
                if mn == "mov" and len(ops_l) == 2 and ops_l[1] in aliases:
                    aliases.add(d)
                else:
                    aliases.discard(d)
        if not zero:
            continue
        if f in dtor or f in sdtor:
            func_class[f] = zero[0]
        else:
            func_class[f] = zero[-1]
            ctor_extent[f] = hi
    # Allocation sites.
    ctor_funcs = {f for f in func_class if f not in dtor and f not in sdtor}
    raw_sites = []
    for i, mn in enumerate(dis.mn):
        if mn != "call" or dis.func_of[i] is None:
            continue
        x = call_target(dis.ops[i])
        if x is None:
            continue
        size = None
        for k in range(i - 1, max(i - 9, -1), -1):
            if dis.mn[k] == "push":
                m = re.fullmatch(r"0x([0-9a-f]+)", dis.ops[k])
                if m:
                    size = int(m.group(1), 16)
                break
            if dis.mn[k] in ("call", "ret", "jmp") or dis.mn[k].startswith("j"):
                break
        if size is None or not (4 <= size <= 0x100000):
            continue
        aliases = {"eax"}
        events = []
        f = dis.func_of[i]
        k = i + 1
        while k < len(dis.mn) and k <= i + 30 and dis.func_of[k] == f:
            mnk, opk = dis.mn[k], dis.ops[k]
            if mnk == "ret" or mnk == "jmp":
                break
            if mnk == "mov":
                m = RE_VFT_STORE.match(opk)
                if m and m.group(1) in aliases and not m.group(2) and int(m.group(3), 16) in vfts \
                        and vfts[int(m.group(3), 16)]["offset"] == 0:
                    events.append((k, int(m.group(3), 16), "inline"))
                    k += 1
                    continue
                p = split_ops(opk)
                if len(p) == 2 and re.fullmatch(r"e[a-z]{2}", p[0]):
                    if p[1] in aliases:
                        aliases.add(p[0])
                    else:
                        aliases.discard(p[0])
            if mnk == "call":
                c = call_target(opk)
                if c in ctor_funcs and "ecx" in aliases:
                    events.append((k, func_class[c], f"ctor {hx(c)}"))
                    aliases = (aliases & {"esi", "edi", "ebx", "ebp"}) | {"eax"}
                elif events:
                    break
                else:
                    break
            k += 1
        if events:
            raw_sites.append((i, x, size, events[-1]))
    alloc_hits = collections.Counter(x for _, x, _, _ in raw_sites)
    allocators = {x for x, n in alloc_hits.items() if n >= 3}
    sites = collections.defaultdict(list)  # vftable -> [(size, site addr, allocator, how)]
    for i, x, size, (k, vt, how) in raw_sites:
        if x in allocators:
            sites[vt].append((size, dis.addr[i], x, how))
    # Static instances: `mov ecx,<data address>` shortly followed by a constructor call (dynamic initialisers).
    statics = collections.defaultdict(set)  # vftable -> {data address}
    for i, mn in enumerate(dis.mn):
        if mn != "mov" or not dis.ops[i].startswith("ecx,0x"):
            continue
        a = int(dis.ops[i][6:], 16)
        if not (DATA_LO <= a <= DATA_HI):
            continue
        for k in range(i + 1, min(i + 4, len(dis.mn))):
            if dis.mn[k] == "call":
                c = call_target(dis.ops[k])
                if c in ctor_funcs:
                    statics[func_class[c]].add(a)
                break
            if dis.ops[k].startswith("ecx"):
                break
    return {"stores": stores, "sdtor": sdtor, "dtor": dtor, "func_class": func_class, "ctor_extent": ctor_extent,
            "sites": sites, "allocators": {x: alloc_hits[x] for x in allocators}, "statics": statics,
            "dealloc": dealloc_count.most_common(3)}


# ---------------------------------------------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------------------------------------------

def harvest_globals(dis: Disasm):
    """Per data address: reads, writes, address-taken uses, widths, indexed use, referencing functions."""
    g = collections.defaultdict(lambda: {"r": set(), "w": set(), "a": set(), "widths": set(), "idx": False,
                                         "fp": False, "n": 0})
    for i, ops in enumerate(dis.ops):
        if "0x" not in ops:
            continue
        f = dis.func_of[i]
        if f is None:
            continue
        mn = dis.mn[i]
        parts = split_ops(ops)
        for pi, op in enumerate(parts):
            if "0x" not in op:
                continue
            mo = mem_operand(op) if ("[" in op or "ds:" in op) else None
            if mo is not None:
                a = mo[2]
                if not (DATA_LO <= a <= DATA_HI) or (mo[1] is not None and mo[2] < 0x10000):
                    continue
                e = g[a]
                e["n"] += 1
                if mn == "lea":
                    e["a"].add(f)
                    continue
                if mo[0]:
                    e["widths"].add(mo[0])
                if mo[1] is not None or mo[3]:
                    e["idx"] = True
                if mn.startswith("f") or mn.endswith("ss") or mn.endswith("sd") or mn.endswith("ps"):
                    e["fp"] = True
                k = access_kind(mn, pi, len(parts))
                if "r" in k:
                    e["r"].add(f)
                if "w" in k:
                    e["w"].add(f)
            else:
                m = re.fullmatch(r"(?:OFFSET )?0x([0-9a-f]+)", op)
                if not m:
                    continue
                a = int(m.group(1), 16)
                if DATA_LO <= a <= DATA_HI and not (pi == 0 and len(parts) > 1):
                    g[a]["a"].add(f)
                    g[a]["n"] += 1
    return g


WRITE_1OP = {"fstp", "fst", "fistp", "fist", "fisttp", "fnstsw", "fnstcw", "fstcw", "pop", "fbstp"}
RMW_1OP = {"inc", "dec", "neg", "not"}


def access_kind(mn: str, pi: int, nops: int) -> str:
    """'r', 'w' or 'rw' for a memory operand at position pi of an instruction with nops operands."""
    if pi != 0:
        return "r"
    if nops == 1:
        if mn in WRITE_1OP or mn.startswith("set"):
            return "w"
        return "rw" if mn in RMW_1OP else "r"
    if mn in RMW:
        return "rw"
    if mn in READ_ONLY_DST:
        return "r"
    return "w"


# ---------------------------------------------------------------------------------------------------------------
# Repo-established structure layouts
# ---------------------------------------------------------------------------------------------------------------

# Two-word and lower-case structure names the repo uses in prose, mapped to one canonical name each.
ALIASES = {
    "instance": "Instance", "Instance": "Instance", "inst": "Instance", "instance data": "Instance",
    "player data": "PlayerData", "Lara data": "PlayerData", "lara data": "PlayerData", "Lara state": "PlayerData",
    "player": "PlayerInstance", "Lara": "PlayerInstance",
    "Level": "Level", "level": "Level", "unit data": "UnitData", "Intro": "Intro", "intro": "Intro",
    "intro data": "IntroData", "gameTracker": "GameTracker", "GameTracker": "GameTracker",
    "mainTracker": "MainTracker", "object data": "ObjectData", "objdata": "ObjectData", "Object data": "ObjectData",
    "ObjectBase": "ObjectBase", "death record": "DeathRecord", "physical record": "PhysicalRecord",
    "physical data": "PhysicalData", "weapon controller": "WeaponController", "settings": "RenderSettings",
    "emitter": "Emitter", "particle": "Particle", "Terrain": "Terrain", "terrain": "Terrain",
    "stream unit": "StreamUnit", "StreamUnit": "StreamUnit", "streamUnit": "StreamUnit",
    "material": "Material", "Material": "Material", "mesh": "Mesh", "model": "Model", "Model": "Model",
    "anim request": "AnimRequest", "clip": "AnimClip", "camera": "Camera", "Camera": "Camera",
    "CameraManager": "CameraManager", "camera manager": "CameraManager", "face": "CollisionFace",
    "markup": "Markup", "Markup": "Markup", "section header": "DrmSectionHeader", "header": "Header",
    "vm frame": "ScriptFrame", "frame record": "FrameRecord", "save": "SaveData", "PCRenderDevice": "PCRenderDevice",
    "render device": "PCRenderDevice", "device": "PCRenderDevice", "batch": "RenderBatch", "light": "Light",
    "PCRenderLight": "PCRenderLight", "SceneLight": "SceneLight", "GlobalInfo": "GlobalInfo",
    "GenericFXObject": "GenericFXObject", "Object": "ObjectData", "object": "ObjectData", "joint": "Joint",
    "capsule": "Capsule", "query": "CollisionQuery", "body": "PhysicsBody", "wheel": "Wheel",
    "script instance": "ScriptInstance", "ScriptInstance": "ScriptInstance", "MultiBody": "MultiBody",
    "multibody": "MultiBody", "blob": "MaterialBlob", "cell group": "CellGroupData", "portal": "Portal",
    "anim": "AnimData", "node": "AnimNode", "controller": "Controller", "box": "MarkupBox", "context": "Context",
    "effect": "EffectRecord", "fx": "EffectRecord", "texture": "Texture", "Texture": "Texture",
}
NAME_STOP = {"The", "This", "That", "When", "Observed", "See", "Since", "Each", "Every", "With", "From", "Then",
             "And", "But", "For", "Its", "Our", "TRU", "DRM", "DTP", "PS", "VS", "RVA", "VA", "ID", "UV", "RGB", "RGBA",
             "PE", "CRC", "FUN", "DAT", "LAB", "PTR", "XMM", "XMMWORD", "DWORD", "WORD", "BYTE", "Offset", "Off"}
RE_PROSE = re.compile(r"((?:[A-Za-z][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)(?: (?:data|record|controller|"
                      r"request|header|manager|unit|instance|frame|device|group|state))?)\s?\+\s?0x([0-9A-Fa-f]{1,5})"
                      r"(?![0-9A-Za-z_])")
RE_FUNCADDR = re.compile(r"(?<![0-9A-Za-z_+])(?:0x|FUN_|DAT_)0{0,2}([4-8][0-9A-Fa-f]{5})(?![0-9A-Za-z_])")
RE_TABLE_OFF = re.compile(r"^`?(?:\+?0x[0-9A-Fa-f]{1,5}|\+[0-9]{1,4})(?![0-9A-Za-z_])")
RE_OFFS_CELL = re.compile(r"(?<![0-9A-Za-z_])(?:\+?0x([0-9A-Fa-f]{1,5})|\+([0-9]{1,4}))(?![0-9A-Za-z_])")


FIRST_STOP = {"to", "the", "and", "with", "its", "from", "at", "of", "in", "on", "by", "over", "within", "when",
              "after", "this", "that", "each", "per", "a", "an", "same", "new", "old", "own", "raw", "base", "next",
              "first", "last", "byte", "word", "dword", "float", "int", "count", "list", "offset", "slot", "flag"}


def canonical(token: str, classes: set[str]):
    if token in ALIASES:
        return ALIASES[token]
    words = token.split(" ")
    head = words[0]
    if len(words) == 2:
        if head.lower() in FIRST_STOP or len(head) < 3 or not head.isalpha():
            return ALIASES.get(words[1]) if words[1] in ("instance", "header", "unit") else None
        if head in ALIASES:
            return None  # e.g. "instance data": ambiguous between the record and its data block
        return "".join(w[:1].upper() + w[1:] for w in words)
    if head in NAME_STOP or len(head) < 3:
        return None
    if token in classes or token.split("::")[-1] in classes:
        return token
    if head[0].isupper() and sum(ch.isupper() for ch in head) >= 2 and any(ch.islower() for ch in head) \
            and "_" not in head:
        return token
    return None


def confidence_of(text: str) -> str:
    """Evidence tag on a repo line: [O]/[OBS]/[V-data]/[V-exe]/Observed, [I]/[INF], [S] (reference source), [U]/[?]."""
    if re.search(r"\[(O|OBS|V-data|V-exe)\]|Observed", text):
        return "observed"
    if re.search(r"\[(I|INF)\]|INFERENCE", text):
        return "inference"
    if "[S]" in text:
        return "reference"
    if re.search(r"\[(U|\?)\]", text):
        return "unknown"
    return "stated"


def repo_files() -> list[str]:
    r = subprocess.run(["git", "-C", str(REPO), "ls-files", *SCAN_ROOTS], capture_output=True, text=True, check=True)
    out = []
    for p in r.stdout.split("\n"):
        if not p or any(p.startswith(s) for s in SKIP_PATHS):
            continue
        if Path(p).suffix in TEXT_EXT or p == "CHANGELOG.md":
            out.append(p)
    return sorted(out)


RE_CPP_STRUCT = re.compile(r"^\s*(?:struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)\b[^;(]*$")
RE_MEMBER_OFF = re.compile(r"//\s*\+0x([0-9A-Fa-f]{1,5})(?![0-9A-Za-z_])")


def harvest_structures(classes: set[str]):
    """(struct, offset) -> evidence from markdown offset tables and `<name> +0x..` prose in code and docs."""
    fields = collections.defaultdict(lambda: {"ev": [], "meaning": "", "src": set(), "conf": set(),
                                              "funcs": set(), "files": set()})
    for p in repo_files():
        try:
            lines = (REPO / p).read_text(encoding="utf-8").split("\n")
        except (UnicodeDecodeError, OSError):
            continue
        heading = ""
        table_name = None
        table_cols: list[str] = []
        cpp_struct = None
        for ln, line in enumerate(lines, 1):
            s = line.strip()
            if p.endswith(".md") and s.startswith("#"):
                heading = s.lstrip("#").strip()
                table_name = None
                continue
            funcs = {int(m, 16) for m in RE_FUNCADDR.findall(line) if TEXT_LO <= int(m, 16) <= TEXT_HI}
            if p.endswith(".md") and s.startswith("|"):
                cells = [c.strip() for c in s.strip("|").split("|")]
                m0 = RE_TABLE_OFF.match(cells[0]) if cells else None
                dec = None
                if not m0 and cells and table_name:
                    dec = re.fullmatch(r"`?(\d{1,5})`?", cells[0])
                if (m0 or dec) and table_name:
                    meaning, evid = [], ""
                    for ci, c in enumerate(cells[1:], 1):
                        col = table_cols[ci] if ci < len(table_cols) else ""
                        if col in ("evidence", "source", "evidence / source", "observed", "notes"):
                            evid = c
                        elif c:
                            meaning.append(c)
                    mean = " | ".join(meaning)
                    offs = [int(dec.group(1))] if dec else [
                        int(m.group(1), 16) if m.group(1) else int(m.group(2))
                        for m in RE_OFFS_CELL.finditer(cells[0].split("(")[0])]
                    for off in offs:
                        f = fields[(table_name, off)]
                        f["ev"].append(f"{p}:{ln}")
                        f["files"].add(p)
                        f["src"].add("table")
                        if not f["meaning"]:
                            f["meaning"] = mean[:200]
                        f["conf"].add(confidence_of(evid + " " + mean))
                        f["funcs"] |= funcs
                if cells and not m0 and ln < len(lines) and re.fullmatch(r"\|[-: |]+\|?", lines[ln].strip()):
                    h = cells[0].strip("`*")
                    hl = h.lower()
                    table_cols = [c.lower().strip("`* ") for c in cells]
                    if not (re.search(r"\boff(set)?\b", hl) or hl in ("field", "rel", "+")):
                        table_name = None
                    else:
                        words = [w for w in re.split(r"\s+", h) if w.lower() not in ("offset", "off", "field",
                                                                                     "rel", "at")]
                        name = " ".join(words).strip()
                        if not name or name.lower() in ("record", "entry", "struct", "address", "addr"):
                            name = (name + " " if name.lower() == "record" else "") + heading
                        name = re.split(r" — | - |: |\(|,|\[", name)[0].replace("`", "")
                        name = re.sub(r"^[0-9.]+\s+", "", name).strip(" `*:")
                        table_name = ALIASES.get(name, name) if name else None
                elif not cells or set(cells[0]) <= set("-: "):
                    continue
            elif p.endswith(".md"):
                table_name = None
            else:
                ms = RE_CPP_STRUCT.match(line)
                if ms and not line.rstrip().endswith(";"):
                    cpp_struct = ms.group(1)
                elif cpp_struct and re.match(r"^\s*};", line):
                    cpp_struct = None
                elif cpp_struct:
                    mc = RE_MEMBER_OFF.search(line)
                    if mc:
                        f = fields[(cpp_struct, int(mc.group(1), 16))]
                        f["ev"].append(f"{p}:{ln}")
                        f["files"].add(p)
                        f["src"].add("mirror")
                        if not f["meaning"]:
                            f["meaning"] = re.sub(r"\s+", " ", s)[:200]
                        f["conf"].add(confidence_of(line))
                        f["funcs"] |= funcs
                        continue
            for m in RE_PROSE.finditer(line):
                name = canonical(m.group(1), classes)
                if not name:
                    continue
                key = (name, int(m.group(2), 16))
                f = fields[key]
                f["ev"].append(f"{p}:{ln}")
                f["files"].add(p)
                f["src"].add("prose")
                if not f["meaning"]:
                    a = max(0, m.start() - 40)
                    f["meaning"] = re.sub(r"\s+", " ", line[a:m.end() + 80].strip(" /*#-"))
                f["conf"].add(confidence_of(line))
                f["funcs"] |= funcs
    return fields


def disp_users(dis: Disasm, func: int, disp: int) -> list[int]:
    """Instruction indices in a function that use a register-based memory operand with this displacement."""
    tok = f"+0x{disp:x}]" if disp else "]"
    out = []
    for i in dis.body.get(func, []):
        ops = dis.ops[i]
        if tok not in ops:
            continue
        for op in split_ops(ops):
            mo = mem_operand(op)
            if mo and mo[1] is not None and mo[2] == disp and mo[1] != "esp":
                out.append(i)
                break
    return out


# ---------------------------------------------------------------------------------------------------------------
# Instruction-level checks of repo-established fields. Each names the function the repo cites for the field and
# a regex the instruction using that displacement must satisfy (width / direction / operand kind).
# ---------------------------------------------------------------------------------------------------------------

CHECKS: list[tuple[str, int, int, str, str]] = [
    # INSTANCE_IntroduceInstance 0x5BCC30: edi = intro (arg 1, 0x5BCC3C), esi = the new instance.
    ("Intro", 0x00, 0x5BCC30, r"^mov eax,DWORD PTR \[edi\]$", "rotation float4 copied dword-wise to instance +0x30"),
    ("Intro", 0x10, 0x5BCC30, r"^mov edx,DWORD PTR \[edi\+0x10\]$", "position float4 copied to instance +0x20"),
    ("Intro", 0x50, 0x5BCC30, r"^movzx eax,WORD PTR \[edi\+0x50\]$", "u16 object id"),
    ("Intro", 0x52, 0x5BCC30, r"^mov dx,WORD PTR \[edi\+0x52\]$", "u16 copied to instance +0x40"),
    ("Intro", 0x58, 0x5BCC30, r"^mov esi,DWORD PTR \[edi\+0x58\]$", "i32 unique intro id (first use: lookup arg)"),
    ("Intro", 0x71, 0x5BCC30, r"^test BYTE PTR \[edi\+0x71\],0x10$", "flag byte, bit 0x10 = use the +0x40 scale"),
    ("Instance", 0x30, 0x5BCC30, r"^mov DWORD PTR \[esi\+0x30\],eax$", "rotation (from intro +0x00)"),
    ("Instance", 0x40, 0x5BCC30, r"^mov WORD PTR \[esi\+0x40\],dx$", "u16 from intro +0x52"),
    ("Instance", 0xE0, 0x5B3A20, r"^mov DWORD PTR \[edi\+0xe0\],ebx$", "unit pointer set by 0x5B3A20"),
    ("Instance", 0x100, 0x5CD1B0, r"^mov eax,DWORD PTR \[esi\+0x100\]$",
     "intro pointer; next instruction loads [eax+0x78] = intro data"),
    ("Instance", 0x1B4, 0x52C920, r"^fstp DWORD PTR \[esi\+0x1b4\]$", "float instance fade"),
    ("Instance", 0x2A0, 0x5DB370, r"^movups XMMWORD PTR \[eax\+0x2a0\],xmm2$", "float4 velocity written back"),
    ("Instance", 0x23C, 0x5B3DD0, r"^(and|or) DWORD PTR \[ecx\+0x23c\],", "u32 hidden draw-group mask"),
    ("Level", 0x74, 0x5B3860, r"^mov edi,DWORD PTR \[edx\+0x74\]$", "unit data pointer"),
    ("CollisionFace", 0x0E, 0x5D3BA0, r"^movzx ecx,BYTE PTR \[esi\+0xe\]$", "u8 client flags"),
    ("RenderSettings", 0x38, 0x513BE0, r"^fld DWORD PTR \[ebx\+0x38\]$", "float far plane"),
    ("Header", 0x15, 0x515B40, r"BYTE PTR \[e[a-d]x\+0x15\]", "u8 mip levels - 1 (texture header)"),
    ("CameraManager", 0x54, 0x7767C0, r"^mov edx,DWORD PTR \[edx\+0x54\]$",
     "MISMATCH: edx = [eax] = the manager's vftable (0x7767CB); +0x54 is vfunc slot 21 called at 0x7767D8 with "
     "(arg, 1) - Script.cpp:1989 says so, Game.hpp:1178 calls it the orbit camera's mode field"),
    ("CameraManager", 0xE4, 0x7759F0, r"^mov eax,DWORD PTR \[edx\+0xe4\]$",
     "vfunc slot 57 (edx = vftable at 0x7759F7) called with 8 at 0x775A06: a call, not a 'type' field as "
     "ENGINE_ARCHITECTURE.md:261 words it; Camera.cpp:15 writes it as a call"),
    ("GameTracker", 0xB8, 0x5E0050, r"^mov al,BYTE PTR \[esi\+0xb8\]$",
     "PARTIAL: u8 state; the caller skips 0x5DAB40 when it is 0 or 8 and 0x5DAB40 tests == 0xC at 0x5DAB48 "
     "(FUNCTIONS.md:36 says 'taken when != 0'); the global copy 0xE7F140 is written 0/8/0xA, compared with 3"),
]


def run_checks(dis: Disasm):
    out = {}
    for struct, off, func, rx, note in CHECKS:
        users = disp_users(dis, func, off)
        hits = [i for i in users if re.search(rx, f"{dis.mn[i]} {dis.ops[i]}")]
        if hits:
            verdict = "mismatch(semantic)" if note.startswith("MISMATCH") else (
                "partial" if note.startswith("PARTIAL") else "match")
            res = verdict + " " + "; ".join(dis.text(i) for i in hits[:2])
        elif users:
            res = "mismatch (disp used, pattern not: " + dis.text(users[0]) + ")"
        else:
            res = "mismatch (no [reg+0x%x] in %s)" % (off, hx(func))
        out[(struct, off)] = (hx(func), note, res)
    return out


# ---------------------------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------------------------

# Meanings for high fan-in globals established in this pass from instructions (address: (name, evidence)).
# Repo citations name a few more; these fill the `meaning` column only.
GLOBAL_NOTES = {
    0xCD9CAC: ("memory manager pointer", "set to static object 0xCDB538 by 0x59A1E0; operator new 0x574940 = "
               "[[p+0x48]].vfunc+0x34(size, 0), delete 0x596B10 = vfunc+0x38(ptr) [O]"),
    0xCC5038: ("debug/log channel", "first argument of the printf-style 0x6457A0 with format strings such as "
               "'NtCell::SetEnabled(%s) => ...' (0x4A4AA8) [O]"),
    0x8BD7E8: ("__security_cookie", "447 /GS prologs `mov eax,[0x8BD7E8]; xor eax,esp` (0x411896) [O]"),
    0xCDA0C4: ("free-list head", "inlined push pattern [eax]=head; head=eax in ~160 functions (0x453ECC) [O]; "
               "which pool [U]"),
    0xFADB58: ("player data pointer", "written by player init 0x721F80 [O]; *(p+0x500) = larabase slots "
               "(GAMEPLAY.md:912) [I]"),
    0xE7F094: ("gameTracker+0x0C player instance", "written by 0x5D3DF0/0x5D40C0 [O]; read as Lara's instance "
               "(GAMEPLAY.md:271) [I]"),
    0xE7EE50: ("GlobalInfo pointer", "GAMEPLAY.md:779 (*0xE7EE50 + 0x194 reward table) [O per repo]"),
    0xC59264: ("game state / options pointer", "GAMEPLAY.md:4022 (+0x3EF94.. difficulty presets) [O per repo]"),
    0xFADFB0: ("aim controller", "GAMEPLAY.md:1899 (cond 0x218 = byte +0x50+0x195) [per repo]"),
    0xA02B68: ("InputSystem*", "RESEARCH.md:277 [per repo]"),
    0x8A9C78: ("lagged-Fibonacci RNG state", "src/game/Feedback.hpp:120 (0x597780) [per repo]; 640 bytes to next ref"),
}


# Statically allocated structures whose code-referenced addresses become structures.tsv fields
# (name: (base, scan length)). Lengths are where the dense run of references ends [I], not proven sizes.
GLOBAL_STRUCTS = {
    "GameTracker": (0xE7F088, 0x184),   # references dense up to 0xE7F208; 0xE7F210.. is a 0x10-stride run [U]
    "MainTracker": (0xE7ED60, 0x70),    # last reference 0xE7EDC8 before g_frameDeltaRing 0xE7EE10
}


def namespace_or_root(name: str, root: str) -> str:
    if "::" in name and not name.startswith("`"):
        ns = name.split("<")[0]
        if "::" in ns:
            return ns.split("::")[0] + "::"
    return root


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", type=Path, default=Path(os.environ.get("OU_GAME_DIR", "/mnt/1st4TB/Tomb Raider Underworld"))
                    / "tru.exe")
    ap.add_argument("--work", type=Path, default=REPO.parent / "openunderworld-work")
    ap.add_argument("--out", type=Path, default=REPO / "docs" / "re" / "db")
    a = ap.parse_args()
    fdir = a.work / "re-baseline" / "FunctionDb"
    gdir = a.work / "ghidra" / "exports"
    ddir = a.work / "re-baseline" / "FuncDetail"
    db = REPO / "docs" / "re" / "db"

    pe = PE(a.exe)
    rtti = Rtti(pe)
    dis = Disasm(a.exe, fdir / "functions_full.tsv")
    obj = analyse_objects(dis, rtti)
    funcs_db = {int(r["entry"], 16): r for r in read_tsv(db / "functions.tsv")}
    rtti_db = {int(r["vftable"], 16): r for r in read_tsv(db / "rtti.tsv")}
    ghidra_rtti = {int(r["vftable"], 16): r for r in read_tsv(gdir / "rtti.tsv")}
    cites = collections.defaultdict(list)
    cite_names = {}
    for r in read_tsv(db / "citations.tsv"):
        ad = int(r["address"], 16)
        cites[ad].append(f"{r['path']}:{r['line']}")
        if r["name"] and r["kind"] in ("data", "data_symbol", "vftable") and ad not in cite_names:
            cite_names[ad] = r["name"]
    summary: dict = {}

    # ---- classes / hierarchy ----
    by_td = collections.defaultdict(list)
    for vt, v in rtti.vft.items():
        by_td[v["td"]].append(vt)
    td_chd = {}
    for c in rtti.col.values():
        td_chd.setdefault(c["td"], c["chd"])
    ctor_by_vt = collections.defaultdict(list)
    dtor_by_vt = collections.defaultdict(list)
    for f, vt in obj["func_class"].items():
        (dtor_by_vt if (f in obj["dtor"] or f in obj["sdtor"]) else ctor_by_vt)[vt].append(f)
    sdtor_by_vt = collections.defaultdict(list)
    for vt, v in rtti.vft.items():
        for s in v["slots"]:
            if s in obj["sdtor"]:
                sdtor_by_vt[vt].append(s)
    size_of = {}
    class_rows = []
    hier_rows = []
    families = collections.Counter()
    in_bca = {b["td"] for c in rtti.chd.values() for b in c["bases"][1:]}
    tds = sorted(rtti.td, key=lambda t: (rtti.name(t), rtti.td[t]))
    for td in tds:
        name = rtti.name(td)
        vts = sorted(by_td.get(td, []), key=lambda v: (rtti.vft[v]["offset"], v))
        prim = next((v for v in vts if rtti.vft[v]["offset"] == 0), None)
        chd = rtti.chd.get(td_chd.get(td))
        bases = chd["bases"] if chd else []
        tree = hierarchy_tree(bases) if bases else []
        direct = [rtti.name(bases[i]["td"]) for i, parent, _ in tree if parent == 0]
        root = name
        k = 0
        while k + 1 < len(tree) and tree[k + 1][1] == k:
            k += 1
            root = rtti.name(bases[k]["td"])
        fam = namespace_or_root(name, root)
        if chd or vts:
            families[fam] += 1
        for i, parent, depth in tree[1:]:
            b = bases[i]
            hier_rows.append([name, rtti.name(b["td"]), rtti.name(bases[parent]["td"]) if parent >= 0 else "", depth,
                              b["mdisp"], b["pdisp"], b["vdisp"], f"{b['attr']:x}", chd["attr"]])
        sizes = collections.defaultdict(list)
        for v in vts:
            for sz, site, alloc, how in obj["sites"].get(v, []):
                sizes[sz].append(f"{site:x}:{alloc:x}:{how.split()[0]}")
        ctors = sorted({f for v in vts for f in ctor_by_vt.get(v, [])})
        dtors = sorted({f for v in vts for f in dtor_by_vt.get(v, [])})
        sdt = sorted({f for v in vts for f in sdtor_by_vt.get(v, [])})
        minsz = max([obj["ctor_extent"].get(f, 0) for f in ctors] or [0])
        if len(sizes) == 1:
            sz = next(iter(sizes))
            conf = "high" if len(sizes[sz]) >= 2 else "medium"
            if minsz > sz:
                conf = "conflict(ctor writes past size)"
            size_of[td] = sz
            sz_s = f"0x{sz:x}"
        elif sizes:
            sz_s = "/".join(f"0x{s:x}" for s in sorted(sizes))
            conf = "conflict"
        else:
            sz_s, conf = "", ""
        ev = [e for s in sorted(sizes) for e in sizes[s]][:4]
        slots = len(rtti.vft[prim]["slots"]) if prim else ""
        cited = rtti_db.get(prim, {}).get("slots_cited", "") if prim else ""
        statics = sorted({s for v in vts for s in obj["statics"].get(v, [])})
        class_rows.append([
            name, rtti.td[td], hx(td), hx(prim) if prim else "",
            ";".join(f"{hx(v)}@{rtti.vft[v]['offset']}" for v in vts if v != prim), slots, cited,
            chd["attr"] if chd else "", len(bases) - 1 if bases else "", ";".join(direct), fam,
            sz_s, conf, ";".join(ev), f"0x{minsz:x}" if minsz else "", ";".join(hx(f) for f in ctors[:6]),
            ";".join(hx(f) for f in dtors[:4]), ";".join(hx(f) for f in sdt[:4]),
            ";".join(hx(s) for s in statics[:4]),
            ("yes" if prim in ghidra_rtti else "no") if prim else ("base-only" if td in in_bca else "other-only"),
        ])
    # Base-vs-derived size sanity check.
    size_conflicts = []
    for td, sz in size_of.items():
        chd = rtti.chd.get(td_chd.get(td))
        for b in (chd["bases"][1:] if chd else []):
            bs = size_of.get(b["td"])
            if bs is not None and bs > sz:
                size_conflicts.append(f"{rtti.name(td)} 0x{sz:x} < base {rtti.name(b['td'])} 0x{bs:x}")
    write_tsv(a.out / "classes.tsv",
              ["class", "mangled", "type_descriptor", "vftable", "secondary_vftables", "slots", "slots_cited",
               "chd_attributes", "n_bases", "direct_bases", "family", "sizeof", "sizeof_confidence",
               "sizeof_evidence", "ctor_min_size", "constructors", "destructors", "deleting_dtors",
               "static_instances", "ghidra_rtti"], class_rows)
    write_tsv(a.out / "class_hierarchy.tsv",
              ["class", "base", "via", "depth", "mdisp", "pdisp", "vdisp", "bcd_attributes", "chd_attributes"],
              hier_rows)
    summary["classes"] = {
        "type_descriptors": len(rtti.td), "complete_object_locators": len(rtti.col), "vftables": len(rtti.vft),
        "hierarchy_descriptors": len(rtti.chd), "hierarchy_rows": len(hier_rows),
        "vftables_in_ghidra_rtti_tsv": sum(1 for v in rtti.vft if v in ghidra_rtti),
        "ghidra_rtti_rows": len(ghidra_rtti),
        "ghidra_slot_count_mismatch": sum(1 for v, r in ghidra_rtti.items()
                                          if v in rtti.vft and int(r["nslots"]) != len(rtti.vft[v]["slots"])),
        "multiple_inheritance": sum(1 for c in rtti.chd.values() if c["attr"] & 1),
        "virtual_inheritance": sum(1 for c in rtti.chd.values() if c["attr"] & 2),
        "secondary_vftables": sum(1 for v in rtti.vft.values() if v["offset"]),
        "families": dict(sorted(families.items(), key=lambda kv: (-kv[1], kv[0]))[:40]),
        "vftable_storing_functions": len(obj["stores"]), "constructors": sum(len(v) for v in ctor_by_vt.values()),
        "destructors": len(obj["dtor"]), "deleting_destructors": len(obj["sdtor"]),
        "allocators": {hx(k): v for k, v in sorted(obj["allocators"].items())},
        "deallocators": [[hx(x) if x else "", n] for x, n in obj["dealloc"]],
        "sizeof_recovered": len(size_of),
        "sizeof_high": sum(1 for r in class_rows if r[12] == "high"),
        "sizeof_medium": sum(1 for r in class_rows if r[12] == "medium"),
        "sizeof_conflict": sum(1 for r in class_rows if r[12].startswith("conflict")),
        "ctor_min_size_only": sum(1 for r in class_rows if r[14] and not r[11]),
        "classes_with_static_instances": sum(1 for r in class_rows if r[18]),
        "base_larger_than_derived": size_conflicts,
    }
    vft_stores_fd = ddir / "vft_stores.tsv"
    if vft_stores_fd.exists():
        fd = set()
        for r in read_tsv(vft_stores_fd):
            if re.fullmatch(r"[0-9a-f]{8}", r.get("entry") or "") and re.fullmatch(r"[0-9a-f]{8}", r.get("value") or ""):
                if int(r["value"], 16) in rtti.vft:
                    fd.add((int(r["entry"], 16), int(r["value"], 16)))
        mine = {(f, s[3]) for f, st in obj["stores"].items() for s in st}
        summary["classes"]["vft_stores_vs_funcdetail"] = {"objdump": len(mine), "funcdetail": len(fd),
                                                          "both": len(mine & fd)}

    # ---- globals ----
    g = harvest_globals(dis)
    gfd = collections.defaultdict(lambda: {"r": set(), "w": set(), "a": set()})
    gpath = ddir / "globals.tsv"
    if gpath.exists():
        for r in read_tsv(gpath):
            if not (re.fullmatch(r"[0-9a-f]{8}", r.get("address") or "")
                    and re.fullmatch(r"[0-9a-f]{8}", r.get("entry") or "")):
                continue
            ad = int(r["address"], 16)
            if not (DATA_LO <= ad <= DATA_HI):
                continue
            e = int(r["entry"], 16)
            for k, col in (("r", "reads"), ("w", "writes"), ("a", "addr_taken")):
                if int(r[col] or 0):
                    gfd[ad][k].add(e)
    imports = {}
    for r in read_tsv(gdir / "imports.tsv"):
        if r["iat"]:
            imports[int(r["iat"], 16)] = f"{r['library']}!{r['name']}"
    strings = {int(r["address"], 16): r["value"] for r in read_tsv(gdir / "strings.tsv")}
    user_syms = {int(r["address"], 16): r["name"] for r in read_tsv(fdir / "data_symbols.tsv")
                 if r["source"] == "USER_DEFINED"}
    csv_names = {}
    with (REPO / "tools" / "ghidra" / "names" / "tru_gog.csv").open(encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("0x"):
                p = line.split(",")
                v = int(p[0], 16)
                if DATA_LO <= v <= DATA_HI:
                    csv_names[v] = p[1]
    rtti_addrs = set(rtti.td) | set(rtti.col) | set(rtti.chd)
    for c in rtti.chd.values():
        rtti_addrs.add(c["va"])
    static_cls = {}
    for vt, ss in obj["statics"].items():
        for s in ss:
            static_cls[s] = rtti.name(rtti.vft[vt]["td"])
    addrs = sorted(set(g) | set(gfd))
    named = {}
    for ad in addrs:
        nm = user_syms.get(ad) or csv_names.get(ad) or cite_names.get(ad) or ""
        if nm:
            named[ad] = nm
    named_sorted = sorted(named)
    grows = []
    kinds = collections.Counter()
    for n, ad in enumerate(addrs):
        e = g.get(ad)
        f = gfd.get(ad)
        r = (e["r"] if e else set()) | (f["r"] if f else set())
        w = (e["w"] if e else set()) | (f["w"] if f else set())
        at = (e["a"] if e else set()) | (f["a"] if f else set())
        allf = r | w | at
        sec = pe.section_name(ad)
        if ad in imports:
            kind = "import"
        elif ad in rtti.vft:
            kind = "vftable"
        elif ad in rtti_addrs:
            kind = "rtti"
        elif ad in strings:
            kind = "string"
        elif ad in static_cls:
            kind = "object"
        elif e and e["fp"] and not w and sec == ".rdata":
            kind = "float-const"
        elif sec == ".rdata":
            kind = "rdata"
        else:
            kind = "data"
        kinds[kind] += 1
        nxt = addrs[n + 1] if n + 1 < len(addrs) else ad + 4
        s = pe.sec(ad)
        gap = min(nxt, s[2] if s else nxt) - ad
        nm = named.get(ad, "")
        meaning = " — ".join(GLOBAL_NOTES[ad]) if ad in GLOBAL_NOTES else ""
        if not nm:
            if kind == "vftable":
                nm = rtti.name(rtti.vft[ad]["td"]) + "::vftable"
            elif kind == "import":
                nm = imports[ad]
            elif kind == "string":
                nm = '"' + strings[ad][:40] + '"'
            elif kind == "object":
                nm = "static " + static_cls[ad]
        near = ""
        if not nm:
            k = bisect.bisect_left(named_sorted, ad) - 1
            if k >= 0 and ad - named_sorted[k] <= 0x400:
                near = f"{named[named_sorted[k]]}+0x{ad - named_sorted[k]:x}"
        subs = collections.Counter()
        for fn in allf:
            sb = funcs_db.get(fn, {}).get("subsystem", "").lstrip("~")
            lib = funcs_db.get(fn, {}).get("library", "")
            if lib:
                sb = sb or "lib:" + lib.split(":")[0]
            if sb:
                subs[sb] += 1
        objd = (e["r"] | e["w"] | e["a"]) if e else set()
        ghid = (f["r"] | f["w"] | f["a"]) if f else set()
        grows.append([hx(ad), sec, kind, nm, near, gap, ";".join(str(x) for x in sorted(e["widths"])) if e else "",
                      "1" if e and e["idx"] else "", len(r), len(w), len(at), len(allf), e["n"] if e else 0,
                      len(objd), len(ghid), len(objd & ghid),
                      ";".join(f"{k}:{v}" for k, v in sorted(subs.items(), key=lambda kv: (-kv[1], kv[0]))[:3]),
                      ";".join(hx(x) for x in sorted(w)[:3]), ";".join(hx(x) for x in sorted(r)[:3]),
                      ";".join(cites.get(ad, [])[:2]), meaning])
    write_tsv(a.out / "globals.tsv",
              ["address", "section", "kind", "name", "nearest_named", "gap_to_next", "access_widths", "indexed",
               "reader_funcs", "writer_funcs", "addr_taken_funcs", "funcs", "insn_refs", "objdump_funcs",
               "ghidra_funcs", "both_funcs", "subsystems", "writers", "readers", "cited", "meaning"], grows)
    data_rows = [r for r in grows if r[2] in ("data", "object")]
    summary["globals"] = {
        "addresses": len(grows), "kinds": dict(sorted(kinds.items())),
        "by_section": dict(sorted(collections.Counter(r[1] for r in grows).items())),
        "data_written": sum(1 for r in data_rows if r[9]),
        "data_named": sum(1 for r in data_rows if r[3]),
        "objdump_only": sum(1 for ad in addrs if ad in g and ad not in gfd),
        "ghidra_only": sum(1 for ad in addrs if ad in gfd and ad not in g),
        "funcdetail_globals_used": gpath.exists(),
        "top_data_by_fanin": [[r[0], r[3] or r[4], r[11], r[8], r[9], r[16]] for r in
                              sorted(data_rows, key=lambda r: (-r[11], r[0]))[:40]],
    }

    # ---- structures ----
    summary["global_structs"] = {k: [hx(b), hex(s)] for k, (b, s) in GLOBAL_STRUCTS.items()}
    classes = {rtti.name(t) for t in rtti.td} | {rtti.name(t).split("::")[-1] for t in rtti.td}
    fields = harvest_structures(classes)
    # Statically allocated structures: every code-referenced address inside them is a field observed in code.
    for sname, (base, size) in GLOBAL_STRUCTS.items():
        for ad in addrs:
            if not (base <= ad < base + size):
                continue
            e = g.get(ad)
            fg = gfd.get(ad)
            r = (e["r"] if e else set()) | (fg["r"] if fg else set())
            w = (e["w"] if e else set()) | (fg["w"] if fg else set())
            at = (e["a"] if e else set()) | (fg["a"] if fg else set())
            f = fields[(sname, ad - base)]
            f["src"].add("code-ref")
            f["conf"].add("observed")
            nm = named.get(ad, "")
            widths = "/".join(str(x) for x in sorted(e["widths"])) if e and e["widths"] else "?"
            desc = f"{hx(ad)}{' ' + nm if nm else ''}: width {widths}, {len(r)} readers, {len(w)} writers" + (
                f", {len(at)} address-taken" if at else "")
            f["meaning"] = desc + ("; " + f["meaning"] if f["meaning"] else "")
            for c in cites.get(ad, []):
                f["ev"].append(c)
                f["files"].add(c.split(":")[0])
            if not f["ev"]:
                f["ev"].append("globals.tsv:" + hx(ad))
    checks = run_checks(dis)
    srows = []
    rank = {"observed": 0, "inference": 1, "reference": 2, "stated": 3, "unknown": 4}
    for (struct, off) in sorted(fields, key=lambda k: (k[0].lower(), k[0], k[1])):
        f = fields[(struct, off)]
        funcs = sorted(f["funcs"])
        hit = sum(1 for fn in funcs if disp_users(dis, fn, off)) if funcs else 0
        best = min(f["conf"], key=lambda c: rank[c]) if f["conf"] else "stated"
        ck = checks.get((struct, off))
        srows.append([struct, f"0x{off:x}", f["meaning"], ";".join(sorted(f["src"])), best, len(f["ev"]),
                      len(f["files"]), ";".join(f["ev"][:3]), ";".join(hx(x) for x in funcs[:5]),
                      f"{hit}/{len(funcs)}" if funcs else "", f"{ck[0]} {ck[2]} — {ck[1]}" if ck else ""])
    missing = [k for k in checks if k not in fields]
    for (struct, off) in missing:
        ck = checks[(struct, off)]
        srows.append([struct, f"0x{off:x}", ck[1], "check", "observed", 0, 0, "", ck[0], "", f"{ck[0]} {ck[2]}"])
    write_tsv(a.out / "structures.tsv",
              ["struct", "offset", "meaning", "sources", "confidence", "evidence_count", "files", "evidence",
               "cited_funcs", "disp_in_cited_funcs", "asm_check"], srows)
    summary["structures"] = {
        "fields": len(srows), "structs": len({r[0] for r in srows}),
        "by_source": dict(sorted(collections.Counter(s for r in srows for s in r[3].split(";")).items())),
        "by_confidence": dict(sorted(collections.Counter(r[4] for r in srows).items())),
        "with_cited_funcs": sum(1 for r in srows if r[8]),
        "disp_found_in_a_cited_func": sum(1 for r in srows if r[9] and not r[9].startswith("0/")),
        "top_structs": dict(sorted(collections.Counter(r[0] for r in srows).items(),
                                   key=lambda kv: (-kv[1], kv[0]))[:40]),
        "checks": len(checks), "checks_match": sum(1 for c in checks.values() if c[2].startswith("match")),
    }
    (a.out / "structures_summary.json").write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n",
                                                   encoding="utf-8")
    c = summary["classes"]
    print(f"classes: {c['type_descriptors']} TDs, {c['vftables']} vftables, sizeof {c['sizeof_recovered']}; "
          f"globals: {len(grows)}; structures: {len(srows)} fields, checks "
          f"{summary['structures']['checks_match']}/{len(checks)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
