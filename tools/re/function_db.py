#!/usr/bin/env python3
"""Build the tru.exe function database: Ghidra exports joined with every tru.exe address the repo cites.

Inputs (all read-only):
  <work>/ghidra/exports/{rtti,strings,imports}.tsv    tools/ghidra/analyze.sh exports
  <work>/re-baseline/FunctionDb/{functions_full,edges,func_refs,data_symbols,blocks}.tsv
                                                       tools/ghidra/scripts/ExportGraph.java
  <work>/re-baseline/FuncDetail/{detail,vcalls,vft_stores,string_xrefs,this_offsets,fid_*}.tsv
                                                       tools/ghidra/scripts/ExportFunctionDetail.java (all)
  tools/ghidra/names/tru_gog.csv                       applied names
  git ls-files of src/ tests/ docs/ tools/ CHANGELOG.md  citations

Outputs (docs/re/db/, deterministic, no decompiled code):
  functions.tsv      one row per Ghidra function (calling convention, frame, globals, strings, confidence, ...)
  citations.tsv      one row per (address, citing line)
  priorities.tsv     uncited non-library functions reachable from cited game/render roots, ranked
  rtti.tsv           RTTI class families with slot coverage
  vcall_edges.tsv    resolved virtual call sites (caller, site, class, slot, callee, evidence)
  xrefs_summary.tsv  per function: direct/virtual callers and callees, vftable slots, pointer and data refs
  string_refs.tsv    function -> referenced strings (<= 8 per function, values truncated)
  module_deps.tsv    imported API -> calling functions and their subsystems
  this_offsets.tsv   function -> offsets accessed on 'this' / first stack parameter ([INFERENCE])
  summary.json       counts used by docs/re/FUNCTION_DATABASE.md

Usage: tools/re/function_db.py [--work DIR] [--out DIR]
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import math
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
IMAGE_LO, IMAGE_HI = 0x401000, 0x10D9FFF  # .text .. end of .shad (data globals live up to 0x10D4FFF)
SCAN_ROOTS = ("src", "tests", "docs", "tools", "CHANGELOG.md")
# Skipped: our own outputs, the names CSV (its rows are names, not reconstruction evidence) and the PE inventory,
# whose section table cites RVAs/sizes that would mis-resolve as VAs.
SKIP_PATHS = ("docs/re/db/", "tools/re/function_db.py", "docs/re/FUNCTION_DATABASE.md", "tools/ghidra/names/",
              "docs/re/BINARIES.md", "tools/re/pe_inventory.py")
TEXT_EXT = {".cpp", ".hpp", ".h", ".c", ".glsl", ".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".py", ".sh",
            ".java", ".cmake", ".txt", ".md", ".json", ".csv", ".plan", ".in", ".yml", ".yaml", ".toml", ".lua"}
CODE_EXT = {".cpp", ".hpp", ".h", ".c", ".glsl", ".vert", ".frag", ".comp", ".tesc", ".tese", ".geom", ".py", ".sh",
            ".java", ".cmake", ".txt", ".in", ".yml", ".yaml", ".toml", ".lua"}
# FID signature databases queried one at a time (ExportFunctionDetail fid mode) -> library evidence label.
# tru.exe was built with VS2005, so vsOlder is the primary set; vs2012 is the comparison set.
FID_DBS = {"vsOlder_x86": "fid-vsOlder", "vs2012_x86": "fid-vs2012"}
PURECALL = 0x813237  # __purecall (FID vsOlder single match, score 72.7): the abstract-slot stub, never a dispatch target
# Citations checked at instruction level (objdump of tru.exe); see FUNCTION_DATABASE.md "Verification sample".
VERIFIED = {0x5DA230, 0x5E0190, 0x597E90, 0x69DA80, 0x4EFB20, 0x57CE50, 0x6F1790, 0x7594C0, 0x5570F0, 0x6AD070,
            0x473840,
            # second pass (2026-11-16): camera, climbing, wall jump, combat, AI, save, UI
            0x5E2950, 0x5E2CA0, 0x5D6F00, 0x707C60, 0x5D3BA0, 0x63C9B0, 0x5D9E50, 0x60EF50, 0x617EE0, 0x5DB6A0,
            0x476D80}

RE_HEX = re.compile(r"(?<![0-9A-Za-z_+])0x([0-9A-Fa-f]{6,8})(?![0-9A-Za-z_])")
RE_GHIDRA = re.compile(r"\b(?:FUN|DAT|LAB|thunk_FUN|PTR_[A-Za-z_]*?|switchD)_([0-9A-Fa-f]{8})\b")
RE_BARE = re.compile(r"(?<![0-9A-Za-z_])(00[4-9A-Fa-f][0-9A-Fa-f]{5})(?![0-9A-Za-z_])")
RE_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*")
STOP = {"tru", "exe", "tru.exe", "the", "and", "via", "see", "calls", "call", "from", "into", "with", "then", "also",
        "returns", "return", "sets", "set", "reads", "read", "writes", "per", "frame", "function", "func", "vfunc",
        "vtable", "vftable", "address", "at", "in", "of", "to", "for", "is", "on", "or", "by", "as", "its", "her", "his",
        "this", "that", "which", "where", "when", "while", "observed", "inference", "Observed", "INFERENCE", "note",
        "FUN", "DAT", "LAB", "None", "true", "false", "uint32_t", "int", "float", "const", "auto", "static"}


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def hx(v: int) -> str:
    return f"{v:08x}"


def subsystem_of(path: str) -> str:
    p = path.split("/")
    if p[0] == "src":
        if len(p) > 2 and p[1] == "app":
            return "app/" + p[2] if len(p) > 3 else "app"
        return p[1] if len(p) > 2 else "src"
    if p[0] == "tests":
        if len(p) > 2 and p[1] in ("selftests", "replays", "scenarios"):
            return "tests/" + p[1]
        return "tests"
    if p[0] == "tools":
        return "tools/" + p[1] if len(p) > 2 else "tools"
    if p[0] == "docs":
        if len(p) > 2:
            return "docs/" + p[1]
        return "docs/" + Path(p[1]).stem.lower()
    return Path(path).stem.lower()


# Doc subsystems map onto engine areas for function-level voting.
DOC_AREA = {"docs/rendering": "render", "docs/gameplay": "game", "docs/gameplay_audit": "game", "docs/selftest": "game",
            "docs/research": "re", "docs/re": "re", "docs/formats": "formats", "docs/matrix": "game", "docs/audit": "game",
            "tests/selftests": "game", "tests/replays": "game", "changelog": "changelog"}


def area_of(sub: str) -> str:
    if sub.startswith("app/"):
        return sub[4:] if sub[4:] in ("game", "render") else "app"
    return DOC_AREA.get(sub, sub)


def plausible_constant(v: int) -> bool:
    """True for values that look like flags/colours rather than addresses."""
    if v & 0xFFFF == 0 or v & 0xFFF == 0:  # flags, or page-aligned section/size values
        return True
    b = [(v >> s) & 0xFF for s in (0, 8, 16)]
    return b[0] == b[1] == b[2]


def comment_spans(line: str, ext: str, in_block: bool) -> tuple[list[tuple[int, int]], bool]:
    """Character spans of comments in one line of code (rough, string-unaware), and block state after it."""
    spans = []
    i = 0
    n = len(line)
    if ext in (".py", ".sh", ".cmake", ".txt", ".in", ".yml", ".yaml", ".toml"):
        k = line.find("#")
        return ([(k, n)] if k >= 0 else []), False
    if ext == ".lua":
        k = line.find("--")
        return ([(k, n)] if k >= 0 else []), False
    while i < n:
        if in_block:
            k = line.find("*/", i)
            if k < 0:
                spans.append((i, n))
                return spans, True
            spans.append((i, k + 2))
            i = k + 2
            in_block = False
            continue
        a = line.find("//", i)
        b = line.find("/*", i)
        if a < 0 and b < 0:
            break
        if a >= 0 and (b < 0 or a < b):
            spans.append((a, n))
            break
        in_block = True
        i = b + 2
    if not spans and line.lstrip().startswith("*"):
        spans.append((0, n))
    return spans, in_block


class Image:
    def __init__(self, work: Path):
        fdir = work / "re-baseline" / "FunctionDb"
        gdir = work / "ghidra" / "exports"
        self.blocks = [(int(r["start"], 16), int(r["end"], 16), r["name"], r["rwx"]) for r in read_tsv(fdir / "blocks.tsv")]
        self.funcs: dict[int, dict] = {}
        ranges = []
        for r in read_tsv(fdir / "functions_full.tsv"):
            e = int(r["entry"], 16)
            r["_entry"] = e
            r["_size"] = int(r["size"])
            self.funcs[e] = r
            for rg in r["ranges"].split(";"):
                lo, hi = (int(x, 16) for x in rg.split("-"))
                ranges.append((lo, hi, e))
        ranges.sort()
        self.rlo = [x[0] for x in ranges]
        self.ranges = ranges
        self.refs = {int(r["entry"], 16): r for r in read_tsv(fdir / "func_refs.tsv")}
        self.callees = collections.defaultdict(dict)
        self.callers = collections.defaultdict(dict)
        for r in read_tsv(fdir / "edges.tsv"):
            if ":" in r["callee"]:
                continue  # thunk to an imported (EXTERNAL:) function
            a, b = int(r["caller"], 16), int(r["callee"], 16)
            self.callees[a][b] = r["kind"]
            self.callers[b][a] = r["kind"]
        self.data_syms = {int(r["address"], 16): r["name"] for r in read_tsv(fdir / "data_symbols.tsv")}
        self.vftables = {}
        self.slot_class = collections.defaultdict(set)
        self.rtti = read_tsv(gdir / "rtti.tsv")
        for r in self.rtti:
            vt = int(r["vftable"], 16)
            self.vftables[vt] = r
            for s in filter(None, r["slots"].split(";")):
                self.slot_class[int(s, 16)].add(r["class"])
        self.strings = {}
        self.func_strings = collections.defaultdict(list)
        for r in read_tsv(gdir / "strings.tsv"):
            a = int(r["address"], 16)
            self.strings[a] = r["value"]
            for fe in filter(None, r["functions"].split(";")):
                m = re.search(r"@([0-9a-fA-F]{8})$", fe)
                if m:
                    self.func_strings[int(m.group(1), 16)].append(r["value"])
        self.str_sorted = sorted(self.strings)
        self.imports = read_tsv(gdir / "imports.tsv")
        ddir = work / "re-baseline" / "FuncDetail"
        self.detail = {int(r["entry"], 16): r for r in read_tsv(ddir / "detail.tsv")}
        self.vcalls = read_tsv(ddir / "vcalls.tsv")
        self.vft_stores = collections.defaultdict(list)
        for r in read_tsv(ddir / "vft_stores.tsv"):
            e = int(r["entry"], 16) if r["entry"] != "-" else 0  # '-': code outside any Ghidra function
            self.vft_stores[e].append((int(r["site"], 16), r["base"], int(r["disp"]), int(r["value"], 16)))
        self.str_xrefs = collections.defaultdict(list)
        for r in read_tsv(ddir / "string_xrefs.tsv"):
            self.str_xrefs[int(r["entry"], 16)].append((int(r["string"], 16), int(r["sites"])))
        self.this_offs = collections.defaultdict(list)
        for r in read_tsv(ddir / "this_offsets.tsv"):
            self.this_offs[int(r["entry"], 16)].append(r)
        self.fid = {db: {int(r["entry"], 16): r for r in read_tsv(ddir / f"fid_{db}.tsv")} for db in FID_DBS}
        self.ptr_stores = collections.defaultdict(list)  # global -> [(origin, detail)]
        for r in read_tsv(ddir / "ptr_stores.tsv"):
            self.ptr_stores[int(r["global"], 16)].append((r["origin"], r["detail"]))
        self.obj_calls = collections.defaultdict(set)  # global object -> direct callees with ECX = object
        for r in read_tsv(ddir / "obj_calls.tsv"):
            self.obj_calls[int(r["object"], 16)].add(int(r["callee"], 16))

    def block(self, v: int):
        for lo, hi, name, rwx in self.blocks:
            if lo <= v <= hi:
                return name, rwx
        return None, None

    def containing(self, v: int):
        i = bisect.bisect_right(self.rlo, v) - 1
        while i >= 0 and i >= bisect.bisect_right(self.rlo, v) - 4:
            lo, hi, e = self.ranges[i]
            if lo <= v <= hi:
                return e
            i -= 1
        return None

    def resolve(self, v: int):
        """(kind, function entry or None, label) for an address, or None when it maps to nothing known."""
        if v in self.funcs:
            return "func_entry", v, self.funcs[v]["name"]
        name, rwx = self.block(v)
        if name is None:
            return None
        if "x" in rwx:
            e = self.containing(v)
            return ("func_interior", e, self.funcs[e]["name"]) if e is not None else None
        if v in self.vftables:
            return "vftable", None, self.vftables[v]["class"]
        if v in self.strings:
            return "string", None, ""
        if v in self.data_syms:
            return "data_symbol", None, self.data_syms[v]
        return "data", None, name


def git_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "--", *SCAN_ROOTS], cwd=REPO, check=True, capture_output=True, text=True)
    files = []
    for f in out.stdout.splitlines():
        if any(f.startswith(s) for s in SKIP_PATHS):
            continue
        if Path(f).suffix.lower() in TEXT_EXT or f == "CHANGELOG.md":
            files.append(f)
    return sorted(files)


def context_name(line: str, start: int, end: int) -> str:
    """An identifier the citing text attaches to the address, if any."""
    before = line[:start].rstrip()
    before = re.sub(r"(tru\.exe|tru|\[O\]?|at|=|:|\()\s*$", "", before).rstrip()
    m = re.search(r"([A-Za-z_][\w:~]*)\s*\(?\s*$", before)
    cands = []
    if m:
        cands.append(m.group(1))
    after = line[end:]
    m = re.match(r"\s*[\)\]]?\s*(?:[:=,]|->|\(|-|—|–)?\s*\(?\s*([A-Za-z_][\w:~]*)", after)
    if m:
        cands.append(m.group(1))
    for c in cands:
        if c in STOP or c.lower() in STOP or len(c) < 4:
            continue
        if re.fullmatch(r"(?:FUN|DAT|LAB|thunk_FUN)_[0-9a-fA-F]{8}", c):
            continue
        if "_" in c or "::" in c or re.match(r"[A-Z][a-z0-9]+[A-Z]", c):  # not lowerCamel locals/members
            return c
    return ""


def harvest(img: Image) -> list[dict]:
    cites = []
    for f in git_files():
        ext = Path(f).suffix.lower()
        is_code = ext in CODE_EXT
        is_json = ext == ".json"
        is_doc = not is_code and not is_json
        try:
            text = (REPO / f).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        in_block = False
        for ln, line in enumerate(text.splitlines(), 1):
            spans = []
            if is_code:
                spans, in_block = comment_spans(line, ext, in_block)
            hits = []
            for m in RE_HEX.finditer(line):
                hits.append((int(m.group(1), 16), m.start(), m.end(), "hex"))
            for m in RE_GHIDRA.finditer(line):
                hits.append((int(m.group(1), 16), m.start(), m.end(), "ghidra"))
            if is_doc and f.startswith("docs/"):
                for m in RE_BARE.finditer(line):
                    hits.append((int(m.group(1), 16), m.start(), m.end(), "bare"))
            seen = set()
            for v, s, e, how in hits:
                if not IMAGE_LO <= v <= IMAGE_HI or v in seen:
                    continue
                if how == "hex" and plausible_constant(v):
                    continue
                if is_code and how != "ghidra" and not any(a <= s < b for a, b in spans):
                    continue
                if is_json and how != "ghidra" and not re.search(r"tru|note|\[O", line):
                    continue
                r = img.resolve(v)
                if r is None:
                    continue
                kind, fe, label = r
                if how == "bare" and kind not in ("func_entry", "vftable", "data_symbol"):
                    continue
                if kind == "data" and (is_code or is_json) and not re.search(r"tru|global|DAT_|\[O", line):
                    continue
                seen.add(v)
                cites.append({"address": v, "kind": kind, "function": fe, "path": f, "line": ln,
                              "subsystem": subsystem_of(f), "name": context_name(line, s, e),
                              "context": re.sub(r"\s+", " ", line.strip())[:140]})
    return cites


def fid_accepted(img: Image, e: int) -> tuple[str, dict] | None:
    """(db, row) of the first FID database whose match Ghidra's FID analyzer would accept, else None."""
    for db in FID_DBS:
        r = img.fid[db].get(e)
        if r is not None and r["status"] != "below_multi":
            return db, r
    return None


def library_class(img: Image, use_fid: bool = True) -> dict[int, tuple[str, str]]:
    """entry -> (family, evidence) from direct evidence, then filled across same-family address runs."""
    lib: dict[int, tuple[str, str]] = {}
    crt_name = re.compile(r"^(_|`|FID_conflict:|operator|~type_info|type_info|exception|std::|Unwind@|Catch@|"
                          r"\$|__|thunk_FUN_008[12])")
    for e, r in img.funcs.items():
        name, ns = r["name"], r["namespace"]
        if r["fid"] == "1" or r["source"] == "ANALYSIS":
            lib[e] = ("crt", "ghidra-fid" if r["fid"] == "1" else "ghidra-analysis-name")
        elif r["source"] == "IMPORTED":
            lib[e] = ("crt", "imported-symbol")
        elif ns in ("std", "type_info", "exception", "std::exception", "_LocaleUpdate") or crt_name.match(name) and r["source"] != "USER_DEFINED":
            lib[e] = ("crt", "name-pattern")
    fam_of_class = lambda c: ("crt" if c.split("::")[0] in ("std", "type_info", "exception") or c.startswith("`")
                              else "fmod" if c.startswith("FMOD") else "bfx" if c.startswith("bfx::") else None)
    for slot, classes in img.slot_class.items():
        fams = {fam_of_class(c) for c in classes}
        if len(fams) == 1 and None not in fams and slot in img.funcs and slot not in lib:
            fam = fams.pop()
            # A slot shared with engine classes (e.g. _purecall) is not evidence.
            lib[slot] = (fam, "rtti-vftable")
    jpeg = re.compile(r"JPEG|JFIF|Huffman|Thomas G\. Lane|quantization table", re.I)
    fmod = re.compile(r"\bFMOD|fmod_|fmodex|\.\.\\\\src\\\\fmod|aSfxDsp|foreverb", re.I)
    for e, ss in img.func_strings.items():
        if e in lib or e not in img.funcs:
            continue
        if any(jpeg.search(s) for s in ss):
            lib[e] = ("libjpeg", "string")
        elif any(fmod.search(s) for s in ss):
            lib[e] = ("fmod", "string")
    for e, r in img.funcs.items():
        if e not in lib and r["name"].startswith("bfx_"):
            lib[e] = ("bfx", "name-pattern")
    # libjpeg 6b: tools/re/libjpeg_id.py checks named entry points at instruction level and closes over direct
    # calls / stored method pointers from them; game glue, callers and folded stubs shared with game code are not
    # library code.
    not_lib = set()  # game glue next to libjpeg: kept out of the address-run fill
    for r in read_tsv(REPO / "docs/re/db/libjpeg.tsv"):
        e = int(r["entry"], 16)
        if r["role"] in ("glue", "caller", "shared"):
            not_lib.add(e)
        if r["ghidra"] == "1" and r["role"] in ("api", "method", "util", "lib") and e in img.funcs and e not in lib:
            lib[e] = ("libjpeg", "libjpeg-named" if r["name"] != "-" else "libjpeg-reach")
    # Direct FID evidence (ExportFunctionDetail fid mode: each MSVC signature database queried alone). Both
    # databases hold only Microsoft runtime/SDK libraries, so a match means family crt.
    if use_fid:
        for e in sorted(img.funcs):
            m = fid_accepted(img, e)
            if e not in lib and m is not None:
                lib[e] = ("crt", FID_DBS[m[0]])
    # Fill: an unclassified function whose nearest classified neighbours on both sides (by address) share a family
    # and lie within 0x2000 bytes of each other belongs to that library's object-file run.
    entries = sorted(img.funcs)
    seeds = [e for e in entries if e in lib]
    filled = {}
    for e in entries:
        if e in lib or e in not_lib:
            continue
        i = bisect.bisect_left(seeds, e)
        if 0 < i < len(seeds):
            a, b = seeds[i - 1], seeds[i]
            if lib[a][0] == lib[b][0] and b - a <= 0x2000:
                filled[e] = (lib[a][0], "address-run")
    lib.update(filled)
    return lib


def resolve_vcalls(img: Image) -> tuple[list[tuple], collections.Counter]:
    """Resolve `call [vptr+N]` sites whose receiver class is provable from the instruction stream.

    ExportFunctionDetail traces each site's object register back along straight-line code (no merge points) to:
      store      an immediate vftable written into [obj+vptr_off] before the load (constructor in the same function)
      retof      EAX returned by a direct call F: F's last vftable store into [this+vptr_off] (new + constructor),
                 or F is a getter returning a global object (`mov eax, imm`) / global pointer (`mov eax, [g]`)
      this       ECX at entry (or a callee-saved register written once from entry ECX); the function is a slot of
                 classes C: the receiver's dynamic class is one of the classes whose vftable holds this function
      globalobj  a static object at a fixed address
      globalptr  an object pointer loaded from a global variable
    A static object's class is the most derived class among the vftables stored into it (absolute immediate stores
    and constructors/destructors called with ECX = its address: both only ever store classes of its own chain).
    A global pointer resolves only when every direct store into it is resolvable (null stores ignored); its
    classes are the union. Returns rows (caller, site, class, slot, callee, evidence, candidates) and counts of
    unresolved origins."""
    vt_by_class = collections.defaultdict(list)
    bases_of = collections.defaultdict(set)
    for vt, r in img.vftables.items():
        vt_by_class[r["class"]].append((int(r["offset"]), vt, [int(s, 16) for s in filter(None, r["slots"].split(";"))]))
        bases_of[r["class"]].update(filter(None, r["bases"].split(";")))
    slot_of = collections.defaultdict(list)  # function -> (class, vftable offset in the object)
    for vt, r in img.vftables.items():
        for s in filter(None, r["slots"].split(";")):
            slot_of[int(s, 16)].append((r["class"], int(r["offset"])))
    ctor_vt = collections.defaultdict(dict)  # function -> {disp: last vftable stored into [this+disp]}
    abs_vt = collections.defaultdict(set)  # absolute address -> vftables stored there
    for e, stores in img.vft_stores.items():
        for site, base, disp, val in sorted(stores):
            if val not in img.vftables:
                continue
            if base == "this":
                ctor_vt[e][disp] = val
            elif base.startswith("abs:"):
                abs_vt[int(base[4:], 16)].add(val)

    def target(f):
        r = img.funcs.get(f)
        while r is not None and r["thunk"] == "1" and r["thunk_target"] and ":" not in r["thunk_target"]:
            f = int(r["thunk_target"], 16)
            r = img.funcs.get(f)
        return f

    def most_derived(vts):
        classes = {img.vftables[v]["class"] for v in vts}
        top = [c for c in classes if classes - {c} <= bases_of[c]]
        return [min(v for v in vts if img.vftables[v]["class"] == top[0])] if len(top) == 1 else None

    def global_obj(g, k):
        vts = set(abs_vt.get(g + k, ()))
        for callee in img.obj_calls.get(g, ()):
            vt = ctor_vt.get(target(callee), {}).get(k)
            if vt is not None:
                vts.add(vt)
        return most_derived(vts) if vts else None

    def global_ptr(p, k, depth):
        stores = img.ptr_stores.get(p, [])
        out = set()
        for origin, detail in stores:
            if (origin == "imm" and int(detail, 16) == 0) or (origin == "other" and detail == "XOR"):
                continue  # null store (XOR reg, reg clears the stored register)
            got = obj_vts(origin, detail, k, depth + 1)
            if got is None:
                return None
            out.update(got)
        return sorted(out) if out else None

    def getter(f, k, depth):
        rv = img.detail.get(f, {}).get("ret_value", "")
        if rv.startswith("imm:"):
            return global_obj(int(rv[4:], 16), k)
        if rv.startswith("load:"):
            return global_ptr(int(rv[5:], 16), k, depth)
        return None

    def obj_vts(origin, detail, k, depth=0):
        """vftables the object at this origin can carry at offset k, or None when not provable."""
        if depth > 3:
            return None
        if origin == "store":
            return [int(detail, 16)] if int(detail, 16) in img.vftables else None
        if origin == "imm":
            return global_obj(int(detail, 16), k)
        if origin == "retof":
            f = target(int(detail, 16))
            vt = ctor_vt.get(f, {}).get(k)
            return [vt] if vt is not None else getter(f, k, depth)
        if origin == "globalobj":
            return global_obj(int(detail, 16), k)
        if origin == "globalptr":
            return global_ptr(int(detail, 16), k, depth)
        return None

    evidence = {"store": "ctor-store", "globalobj": "global-object", "globalptr": "global-ptr-stores",
                "this": "this-method"}
    rows, unresolved = [], collections.Counter()
    for v in img.vcalls:
        caller, site, n, k = int(v["entry"], 16), int(v["site"], 16), int(v["slot_off"]) // 4, int(v["vptr_off"])
        origin, detail = v["origin"], v["detail"]
        cands = collections.defaultdict(set)
        if origin == "this":
            vts = []
            for cls, off in slot_of.get(caller, ()):
                vts += [vt for o2, vt, _ in vt_by_class[cls] if o2 == off + k]
        elif origin in ("store", "retof", "globalobj", "globalptr"):
            vts = obj_vts(origin, detail, k) or []
        else:
            unresolved[origin] += 1
            continue
        if origin == "retof":
            f = target(int(detail, 16))
            ev = "new-object" if k in ctor_vt.get(f, {}) else "getter"
        else:
            ev = evidence[origin]
        for vt in vts:
            slots = [int(s, 16) for s in filter(None, img.vftables[vt]["slots"].split(";"))]
            if n < len(slots) and slots[n] in img.funcs:
                cands[slots[n]].add(img.vftables[vt]["class"])
        if len(cands) > 1:
            cands.pop(PURECALL, None)  # an abstract class has no instances
        if not cands or set(cands) == {PURECALL}:
            unresolved[origin + ":no-class" if not cands else origin + ":pure"] += 1
            continue
        for callee in sorted(cands):
            cl = sorted(cands[callee])
            rows.append((caller, site, cl[0] + (f"|+{len(cl) - 1}" if len(cl) > 1 else ""), n, callee,
                         ev if len(cands) == 1 else ev + "-poly", len(cands)))
    return rows, unresolved


def compact_offsets(rows: list[dict]) -> str:
    """this_offsets rows of one function/base -> 'off:rw:sizes ...' (offsets hex)."""
    out = []
    for r in rows:
        rw = ("r" if r["r"] != "0" else "") + ("w" if r["w"] != "0" else "")
        out.append(f'{int(r["offset"], 16):x}:{rw or "-"}:{r["sizes"].replace(";", "/")}')
    return " ".join(out)


def clean_str(s: str, n: int) -> str:
    s = re.sub(r"[\t\r\n\\]", " ", s)
    return s[:n] + ("…" if len(s) > n else "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", type=Path, default=REPO.parent / "openunderworld-work")
    ap.add_argument("--out", type=Path, default=REPO / "docs" / "re" / "db")
    args = ap.parse_args()
    img = Image(args.work)
    cites = harvest(img)
    cites.sort(key=lambda c: (c["address"], c["path"], c["line"]))

    applied = {}
    with (REPO / "tools/ghidra/names/tru_gog.csv").open(encoding="utf-8") as f:
        for line in f.read().splitlines()[1:]:
            if not line.strip() or line.startswith("#"):
                continue
            p = line.split(",", 3)
            applied[int(p[0], 16)] = (p[1].strip(), p[2].strip())

    by_func = collections.defaultdict(list)
    for c in cites:
        if c["function"] is not None:
            by_func[c["function"]].append(c)
    lib_nofid = library_class(img, use_fid=False)
    lib = library_class(img)
    vrows, v_unresolved = resolve_vcalls(img)
    vcallees, vcallers = collections.defaultdict(set), collections.defaultdict(set)
    for caller, _site, _cls, _slot, callee, _ev, _n in vrows:
        vcallees[caller].add(callee)
        vcallers[callee].add(caller)
    vslots = collections.defaultdict(list)  # function -> "Class#slot" (primary vftables only)
    for vt in sorted(img.vftables):
        r = img.vftables[vt]
        for i, s in enumerate(filter(None, r["slots"].split(";"))):
            if r["offset"] == "0":
                vslots[int(s, 16)].append(f'{r["class"]}#{i}')

    rows = []
    status_of = {}
    sub_of = {}
    for e in sorted(img.funcs):
        r = img.funcs[e]
        cs = by_func.get(e, [])
        names = collections.Counter(c["name"] for c in cs if c["name"] and c["kind"] == "func_entry")
        if not names:
            names = collections.Counter(c["name"] for c in cs if c["name"])
        repo_name = names.most_common(1)[0][0] if names else ""
        areas = collections.Counter()
        for c in cs:
            a = area_of(c["subsystem"])
            areas[a] += 3 if c["path"].startswith("src/") else 1
        subsystem = areas.most_common(1)[0][0] if areas else ""
        fam = lib.get(e)
        direct_lib = fam is not None and fam[1] != "address-run"
        code_cited = any(c["path"].startswith(("src/", "tests/")) for c in cs)
        scope = "+".join(s for s in ("src", "tests", "docs", "tools", "changelog")
                         if any(c["path"].split("/")[0].lower().startswith(s) for c in cs))
        if direct_lib:
            status = "Library"
        elif code_cited:
            status = "Reimplemented-cited"
        elif fam is not None and not cs:
            status = "Library"
        elif cs or e in applied or r["source"] == "USER_DEFINED":
            status = "Named-only"
        else:
            status = "Unknown"
        status_of[e] = status
        sub_of[e] = subsystem
        cls = sorted(img.slot_class.get(e, ()))
        d = img.detail.get(e, {})
        ret_imm = [int(x) for x in filter(None, d.get("ret_imm", "").split(";"))]
        purge = max(ret_imm) if ret_imm else -1
        frame_params = int(d.get("frame_params") or 0)
        stack_args = purge if purge > 0 else frame_params
        fm = fid_accepted(img, e) or next(((db, img.fid[db][e]) for db in FID_DBS if e in img.fid[db]), None)
        vs = sorted(set(vslots.get(e, ())))
        rows.append({
            "entry": hx(e), "size": r["_size"], "callers": len(img.callers.get(e, {})),
            "callees": len(img.callees.get(e, {})), "thunk": r["thunk"], "thunk_target": r["thunk_target"],
            "ghidra_name": r["name"] if r["namespace"] == "Global" else f'{r["namespace"]}::{r["name"]}',
            "applied_name": applied.get(e, ("", ""))[0], "repo_name": repo_name, "subsystem": subsystem,
            "cited_by": len(cs), "cited_files": len({c["path"] for c in cs}), "cite_scope": scope,
            "citations": ";".join(f'{c["path"]}:{c["line"]}' for c in cs[:3]),
            "rtti_class": "|".join(cls[:3]) + ("|..." if len(cls) > 3 else ""),
            "library": f"{fam[0]}:{fam[1]}" if fam else "", "data_refs": img.refs.get(e, {}).get("data_refs", ""),
            "string_refs": img.refs.get(e, {}).get("string_refs", ""), "status": status,
            "cc": d.get("own_cc", ""), "cc_ghidra": d.get("ghidra_cc", "").lstrip("_"), "ecx_first": d.get("ecx_first", ""),
            "stack_args": stack_args if d.get("own_cc") != "thunk" else "", "params": stack_args // 4 if stack_args > 0 else 0,
            "ret_imm": d.get("ret_imm", ""), "ret_guess": d.get("ret_guess", ""), "frame": d.get("frame_local", ""),
            "g_reads": d.get("g_reads", ""), "g_writes": d.get("g_writes", ""), "g_addr": d.get("g_addr", ""),
            "strings": d.get("strings", ""), "consts": d.get("consts", ""), "this_offsets": d.get("this_offsets", ""),
            "switches": d.get("switches", ""), "switch_cases": d.get("switch_cases", ""), "icalls": d.get("icalls", ""),
            "vcall_sites": d.get("vcall_sites", ""), "vcallers": len(vcallers.get(e, ())), "vcallees": len(vcallees.get(e, ())),
            "vft_slots": "|".join(vs[:2]) + (f"|+{len(vs) - 2}" if len(vs) > 2 else ""),
            "fid": f'{FID_DBS[fm[0]][4:]}:{fm[1]["status"]}:{clean_str(fm[1]["names"], 48)}' if fm else "",
            "confidence": "",
        })

    # Inferred subsystem for uncited code: majority of cited neighbours (callers and callees).
    for row in rows:
        e = int(row["entry"], 16)
        if row["subsystem"] or row["status"] == "Library":
            continue
        votes = collections.Counter(sub_of[n] for n in list(img.callers.get(e, {})) + list(img.callees.get(e, {}))
                                    if sub_of.get(n))
        if votes:
            row["subsystem"] = "~" + votes.most_common(1)[0][0]
    # Confidence in the repo role: high = cited and the citation verified at instruction level (VERIFIED);
    # medium = cited, or library with direct evidence; low = heuristic only (uncited applied name, address-run
    # library fill, one-hop area hint); none.
    for row in rows:
        e = int(row["entry"], 16)
        fam = lib.get(e)
        if by_func.get(e) and e in VERIFIED:
            row["confidence"] = "high"
        elif by_func.get(e) or (fam is not None and fam[1] != "address-run"):
            row["confidence"] = "medium"
        elif fam is not None or row["applied_name"] or row["subsystem"].startswith("~"):
            row["confidence"] = "low"
        else:
            row["confidence"] = "none"

    args.out.mkdir(parents=True, exist_ok=True)
    cols = list(rows[0].keys())
    with (args.out / "functions.tsv").open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, cols, delimiter="\t", lineterminator="\n", quoting=csv.QUOTE_NONE, escapechar="\\")
        w.writeheader()
        w.writerows(rows)
    with (args.out / "citations.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("address\tkind\tfunction\tpath\tline\tsubsystem\tname\n")
        for c in cites:
            f.write(f'{hx(c["address"])}\t{c["kind"]}\t{hx(c["function"]) if c["function"] is not None else ""}\t'
                    f'{c["path"]}\t{c["line"]}\t{c["subsystem"]}\t{c["name"]}\n')

    # Priorities: forward reachability from cited non-library engine roots.
    root_areas = {"game", "render", "anim", "physics", "collision", "script", "fx", "cine", "scene", "engine", "audio",
                  "ui", "save", "app", "cdc", "formats", "platform"}
    roots = [e for e in img.funcs if status_of[e] == "Reimplemented-cited" and sub_of[e] in root_areas]
    depth = {e: 0 for e in roots}
    root_area = {e: sub_of[e] for e in roots}
    # Virtual dispatch leaves no call edge: the other vftable slots of a class with a cited slot count as one step
    # from that root (roots only; base-class slots shared by many classes would otherwise reach everything).
    vt_peers = collections.defaultdict(set)
    for r in img.rtti:
        slots = [int(s, 16) for s in filter(None, r["slots"].split(";"))]
        for s in slots:
            vt_peers[s].update(slots)
    def reach(use_virtual: bool) -> tuple[dict, dict]:
        depth = {e: 0 for e in roots}
        root_area = {e: sub_of[e] for e in roots}
        q = collections.deque(sorted(roots))
        while q:
            a = q.popleft()
            nxt = set(img.callees.get(a, {}))
            if use_virtual:
                nxt |= vcallees.get(a, set())
            if depth[a] == 0:
                nxt |= vt_peers.get(a, set())
            for b in sorted(nxt):
                if b not in depth and b in img.funcs and status_of.get(b) != "Library":
                    depth[b] = depth[a] + 1
                    root_area[b] = root_area[a]
                    q.append(b)
        return depth, root_area

    def uncited(depth):
        return [e for e, d in depth.items() if d and status_of[e] != "Library" and e not in by_func
                and img.funcs[e]["thunk"] != "1"]

    depth_direct, _ = reach(False)
    reach_direct = uncited(depth_direct)
    depth, root_area = reach(True)
    cand = []
    for e in uncited(depth):
        d = depth[e]
        size = img.funcs[e]["_size"]
        fanin = len(img.callers.get(e, {})) + len(vcallers.get(e, ()))
        score = size * math.sqrt(1 + fanin) / (1 + 0.25 * (d - 1))
        cand.append((round(score, 1), e, size, fanin, d))
    cand.sort(key=lambda x: (-x[0], x[1]))
    reach_uncited = len(cand)
    reach_uncited_bytes = sum(x[2] for x in cand)

    def hint(e):
        ss = sorted({s for s in img.func_strings.get(e, []) if 3 <= len(s) <= 40 and s.isprintable()})
        return ss[0].replace("\t", " ") if ss else ""

    with (args.out / "priorities.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("rank\tentry\tsize\tcallers\tcallees\tdepth\troot_area\tscore\tghidra_name\trtti_class\tstring_refs\tstring_hint\n")
        for i, (score, e, size, fanin, d) in enumerate(cand[:200], 1):
            row = rows[sorted(img.funcs).index(e)]
            f.write(f'{i}\t{hx(e)}\t{size}\t{fanin}\t{row["callees"]}\t{d}\t{root_area[e]}\t{score}\t'
                    f'{row["ghidra_name"]}\t{row["rtti_class"]}\t{row["string_refs"]}\t{hint(e)}\n')

    # RTTI families.
    fam_rows = collections.defaultdict(lambda: {"classes": 0, "slots": set(), "cited": set(), "named": set()})
    cls_rows = []
    for r in img.rtti:
        c = r["class"]
        fam = c.split("::")[0] if "::" in c else ("FMOD" if c.startswith("FMOD") else "(global)")
        slots = {int(s, 16) for s in filter(None, r["slots"].split(";"))}
        own = {s for s in slots if s in img.funcs and lib.get(s, ("",))[0] != "crt"}
        cited = {s for s in slots if status_of.get(s) == "Reimplemented-cited"}
        named = {s for s in slots if s in applied or img.funcs.get(s, {}).get("source") == "USER_DEFINED"}
        vt_cited = int(r["vftable"], 16) in {c["address"] for c in cites if c["kind"] == "vftable"}
        fr = fam_rows[fam]
        fr["classes"] += 1
        fr["slots"] |= own
        fr["cited"] |= cited & own
        fr["named"] |= named & own
        cls_rows.append((c, r["vftable"], len(slots), len(cited), "1" if vt_cited else "0", r["bases"]))
    with (args.out / "rtti.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("class\tvftable\tslots\tslots_cited\tvftable_cited\tbases\n")
        for c in sorted(cls_rows):
            f.write("\t".join(map(str, c)) + "\n")

    # Direct call edges and per-function control-flow summaries (ExportGraph edges.tsv / cfg.tsv), versioned as is.
    fdir = args.work / "re-baseline" / "FunctionDb"
    edge_rows = read_tsv(fdir / "edges.tsv")
    with (args.out / "call_edges.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("caller\tcallee\tkind\tsites\n")
        for r in sorted(edge_rows, key=lambda r: (r["caller"], r["callee"], r["kind"])):
            f.write(f"{r['caller']}\t{r['callee']}\t{r['kind']}\t{r['sites']}\n")
    cfg_rows = read_tsv(fdir / "cfg.tsv")
    cfg_cols = ["entry", "blocks", "edges", "exits", "cyclomatic", "back_edges", "instructions", "returns", "calls",
                "indirect_jumps"]
    with (args.out / "cfg.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("\t".join(cfg_cols) + "\n")
        for r in sorted(cfg_rows, key=lambda r: r["entry"]):
            f.write("\t".join(r[c] for c in cfg_cols) + "\n")
    cfg_stats = {"functions": len(cfg_rows), "blocks": sum(int(r["blocks"]) for r in cfg_rows),
                 "edges": sum(int(r["edges"]) for r in cfg_rows),
                 "with_back_edges": sum(1 for r in cfg_rows if r["back_edges"] != "0"),
                 "with_indirect_jumps": sum(1 for r in cfg_rows if r["indirect_jumps"] != "0"),
                 "cyclomatic_over_20": sum(1 for r in cfg_rows if int(r["cyclomatic"]) > 20),
                 "max_cyclomatic": max((int(r["cyclomatic"]), r["entry"]) for r in cfg_rows)}
    edge_kinds = dict(sorted(collections.Counter(r["kind"] for r in edge_rows).items()))

    # Virtual call edges and per-function cross-reference summary.
    with (args.out / "vcall_edges.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("caller\tsite\tclass\tslot\tcallee\tevidence\tcandidates\n")
        for caller, site, cls, slot, callee, ev, n in sorted(vrows):
            f.write(f"{hx(caller)}\t{hx(site)}\t{cls}\t{slot}\t{hx(callee)}\t{ev}\t{n}\n")
    site_ev = {}
    for caller, site, _c, _s, _callee, ev, _n in vrows:
        site_ev[site] = ev
    vt_count = collections.Counter()
    for vt, r in img.vftables.items():
        for s in set(filter(None, r["slots"].split(";"))):
            vt_count[int(s, 16)] += 1
    with (args.out / "xrefs_summary.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("entry\tcallers\tcallees\tvcallers\tvcallees\tvftables\tptr_refs_to\tdata_refs\tg_reads\tg_writes\tstrings\n")
        for e in sorted(img.funcs):
            d, rf = img.detail.get(e, {}), img.refs.get(e, {})
            f.write(f'{hx(e)}\t{len(img.callers.get(e, {}))}\t{len(img.callees.get(e, {}))}\t{len(vcallers.get(e, ()))}\t'
                    f'{len(vcallees.get(e, ()))}\t{vt_count.get(e, 0)}\t{rf.get("ptr_refs_to", "")}\t{rf.get("data_refs", "")}\t'
                    f'{d.get("g_reads", "")}\t{d.get("g_writes", "")}\t{d.get("strings", "")}\n')
    with (args.out / "string_refs.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("entry\tstring\tsites\tvalue\n")
        for e in sorted(img.str_xrefs):
            for a, sites in sorted(img.str_xrefs[e])[:8]:
                f.write(f"{hx(e)}\t{hx(a)}\t{sites}\t{clean_str(img.strings.get(a, ''), 60)}\n")
    # Imported APIs -> calling functions (direct references plus callers of the import's thunk functions).
    dll_stats = collections.defaultdict(lambda: {"imports": 0, "callers": set()})
    with (args.out / "module_deps.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("dll\timport\tcallers\tsubsystems\tcaller_entries\n")
        for r in sorted(img.imports, key=lambda x: (x["library"].upper(), x["name"])):
            thunks = {int(t, 16) for t in re.findall(r"[0-9a-fA-F]{8}", r["thunks"])}
            callers = set()
            for fe in filter(None, r["functions"].split(";")):
                m = re.search(r"@([0-9a-fA-F]{8})$", fe)
                if m and int(m.group(1), 16) in img.funcs and int(m.group(1), 16) not in thunks:
                    callers.add(int(m.group(1), 16))
            for t in thunks:
                callers |= set(img.callers.get(t, {}))
            subs = collections.Counter()
            for c in callers:
                fam = lib.get(c)
                subs[f"lib:{fam[0]}" if status_of[c] == "Library" and fam else (sub_of[c] or "?")] += 1
            dll = r["library"].upper()
            dll_stats[dll]["imports"] += 1
            dll_stats[dll]["callers"] |= callers
            f.write(f'{dll}\t{r["name"]}\t{len(callers)}\t'
                    f'{";".join(f"{k}:{v}" for k, v in sorted(subs.items(), key=lambda x: (-x[1], x[0])))}\t'
                    f'{";".join(hx(c) for c in sorted(callers)[:8])}{";..." if len(callers) > 8 else ""}\n')
    with (args.out / "this_offsets.tsv").open("w", encoding="utf-8", newline="") as f:
        f.write("entry\tbase\tcount\toffsets\n")
        for e in sorted(img.this_offs):
            by_base = collections.defaultdict(list)
            for r in img.this_offs[e]:
                by_base[r["base"]].append(r)
            for b in sorted(by_base):
                f.write(f"{hx(e)}\t{b}\t{len(by_base[b])}\t{compact_offsets(by_base[b])}\n")

    # Functions without a direct caller: what reaches them instead.
    no_direct = [e for e in sorted(img.funcs) if status_of[e] != "Library" and not img.callers.get(e)]
    why = collections.Counter()
    for e in no_direct:
        if vcallers.get(e):
            why["virtual_caller"] += 1
        elif e in img.slot_class:
            why["vftable_slot"] += 1
        elif int(img.refs.get(e, {}).get("ptr_refs_to") or 0) > 0:
            why["pointer_referenced"] += 1
        elif 0x830000 <= e < 0x83E000:
            why["dynamic_initialiser_run"] += 1
        else:
            why["none_found"] += 1
    # FID: confirmation of the address-run fill and conflicts with repo/analyst names.
    fid_stats = {}
    for db in FID_DBS:
        rows_db = img.fid[db]
        acc = [e for e, r in rows_db.items() if r["status"] != "below_multi" and e in img.funcs]
        fid_stats[db] = {
            "matched": len(rows_db), "accepted": len(acc),
            "status": dict(sorted(collections.Counter(r["status"] for r in rows_db.values()).items())),
            "also_ghidra_fid_bookmark": sum(1 for e in acc if img.funcs[e]["fid"] == "1"),
            "libraries": dict(collections.Counter(r["libraries"].split("|")[0] for r in rows_db.values()).most_common(8)),
        }
    acc_any = {e for e in img.funcs if fid_accepted(img, e)}
    old_run = {e: v[0] for e, v in lib_nofid.items() if v[1] == "address-run"}
    fid_stats["vs2012_only_accepted"] = sorted(hx(e) for e in acc_any
                                               if not (e in img.fid["vsOlder_x86"] and img.fid["vsOlder_x86"][e]["status"] != "below_multi"))
    fid_stats["address_run_before"] = len(old_run)
    fid_stats["address_run_before_by_family"] = dict(sorted(collections.Counter(old_run.values()).items()))
    fid_stats["address_run_confirmed"] = sum(1 for e, fam in old_run.items() if e in acc_any and fam == "crt")
    fid_stats["address_run_contradicted"] = sorted(f"{hx(e)}:{fam}" for e, fam in old_run.items() if e in acc_any and fam != "crt")
    fid_stats["address_run_after"] = sum(1 for v in lib.values() if v[1] == "address-run")
    fid_stats["newly_library"] = sum(1 for e in acc_any if e not in lib_nofid)
    fid_stats["on_code_cited"] = sorted(hx(e) for e in acc_any if any(c["path"].startswith(("src/", "tests/"))
                                                                       for c in by_func.get(e, ())))
    fid_stats["on_named"] = sorted(f'{hx(e)}:{applied[e][0] if e in applied else img.funcs[e]["name"]}' for e in acc_any
                                   if e in applied or img.funcs[e]["source"] == "USER_DEFINED")
    fid_stats["applied_to_project"] = 0  # FID names are recorded in functions.tsv `fid`, never applied as names

    # Summary numbers.
    st = collections.Counter(r["status"] for r in rows)
    st_bytes = collections.Counter()
    for r in rows:
        st_bytes[r["status"]] += r["size"]
    nonlib = [r for r in rows if r["status"] != "Library"]
    nonlib_bytes = sum(r["size"] for r in nonlib)
    cited_bytes = sum(r["size"] for r in nonlib if r["status"] == "Reimplemented-cited")
    libfam = collections.Counter(r["library"].split(":")[0] for r in rows if r["library"])
    libfam_bytes = collections.Counter()
    for r in rows:
        if r["library"]:
            libfam_bytes[r["library"].split(":")[0]] += r["size"]
    libev = collections.Counter(r["library"].split(":")[1] for r in rows if r["library"])
    bysub = collections.defaultdict(lambda: [0, 0])
    for r in rows:
        if r["status"] == "Reimplemented-cited":
            bysub[r["subsystem"]][0] += 1
            bysub[r["subsystem"]][1] += r["size"]
    kinds = collections.Counter(c["kind"] for c in cites)
    addr_kinds = collections.Counter()
    for a, k in {(c["address"], c["kind"]) for c in cites}:
        addr_kinds[k] += 1
    cite_sub = collections.Counter(c["subsystem"] for c in cites)
    citing_files = len({c["path"] for c in cites})
    families = {k: {"classes": v["classes"], "slots": len(v["slots"]), "cited": len(v["cited"]), "named": len(v["named"])}
                for k, v in fam_rows.items()}
    summary = {
        "functions": len(rows), "function_bytes": sum(r["size"] for r in rows), "edges": sum(len(v) for v in img.callees.values()),
        "call_edges": {"rows": len(edge_rows), "by_kind": edge_kinds}, "cfg": cfg_stats,
        "status": dict(sorted(st.items())), "status_bytes": dict(sorted(st_bytes.items())),
        "nonlib_bytes": nonlib_bytes, "cited_bytes": cited_bytes,
        "cited_pct_of_nonlib_bytes": round(100 * cited_bytes / nonlib_bytes, 2) if nonlib_bytes else 0,
        "library_families": dict(sorted(libfam.items())), "library_family_bytes": dict(sorted(libfam_bytes.items())),
        "library_evidence": dict(sorted(libev.items())),
        "by_subsystem": {k: {"functions": v[0], "bytes": v[1]} for k, v in sorted(bysub.items(), key=lambda x: -x[1][1])},
        "citations": len(cites), "citing_files": citing_files, "citation_kinds": dict(sorted(kinds.items())),
        "distinct_addresses_by_kind": dict(sorted(addr_kinds.items())),
        "citations_by_subsystem": dict(cite_sub.most_common()),
        "roots": len(roots), "reachable_uncited": reach_uncited, "reachable_uncited_bytes": reach_uncited_bytes,
        "reachable_uncited_direct_only": len(reach_direct),
        "reachable_uncited_direct_only_bytes": sum(img.funcs[e]["_size"] for e in reach_direct),
        "rtti_families": dict(sorted(families.items(), key=lambda x: -x[1]["classes"])),
        "applied_names": len(applied),
        "fid": fid_stats,
        "vcall": {
            "vptr_call_sites": len(img.vcalls), "indirect_calls": sum(int(d["icalls"] or 0) for d in img.detail.values()),
            "origins": dict(collections.Counter(v["origin"] for v in img.vcalls).most_common()),
            "resolved_sites": len(site_ev), "resolved_sites_by_evidence": dict(sorted(collections.Counter(site_ev.values()).items())),
            "edges": len(vrows), "distinct_caller_callee": sum(len(v) for v in vcallees.values()),
            "callees_reached": len(vcallers), "unresolved": dict(sorted(v_unresolved.items())),
        },
        "no_direct_caller_nonlib": len(no_direct), "no_direct_caller_breakdown": dict(sorted(why.items())),
        "calling_convention": dict(collections.Counter(r["cc"] for r in rows).most_common()),
        "cc_vs_ghidra": dict(sorted(collections.Counter(f'{r["cc_ghidra"]}->{r["cc"]}' for r in rows
                                                        if r["cc_ghidra"] not in ("unknown", "")).items())),
        "ret_guess": dict(collections.Counter(r["ret_guess"] for r in rows).most_common()),
        "confidence": dict(collections.Counter(r["confidence"] for r in rows).most_common()),
        "with_switch": sum(1 for r in rows if r["switches"] not in ("", "0")),
        "with_this_offsets": sum(1 for r in rows if r["this_offsets"] not in ("", "0")),
        "with_strings": sum(1 for r in rows if r["strings"] not in ("", "0")),
        "with_global_writes": sum(1 for r in rows if r["g_writes"] not in ("", "0")),
        "module_deps": {k: {"imports": v["imports"], "calling_functions": len(v["callers"])}
                        for k, v in sorted(dll_stats.items())},
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("functions", "status", "cited_pct_of_nonlib_bytes", "citations",
                                               "citing_files", "distinct_addresses_by_kind", "library_families")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
