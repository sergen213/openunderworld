#!/usr/bin/env python3
"""Build the tru.exe function database: Ghidra exports joined with every tru.exe address the repo cites.

Inputs (all read-only):
  <work>/ghidra/exports/{rtti,strings}.tsv            tools/ghidra/analyze.sh exports
  <work>/re-baseline/FunctionDb/{functions_full,edges,func_refs,data_symbols,blocks}.tsv
                                                       tools/ghidra/scripts/ExportGraph.java
  tools/ghidra/names/tru_gog.csv                       applied names
  git ls-files of src/ tests/ docs/ tools/ CHANGELOG.md  citations

Outputs (docs/re/db/, deterministic, no decompiled code):
  functions.tsv   one row per Ghidra function
  citations.tsv   one row per (address, citing line)
  priorities.tsv  uncited non-library functions reachable from cited game/render roots, ranked
  rtti.tsv        RTTI class families with slot coverage
  summary.json    counts used by docs/re/FUNCTION_DATABASE.md

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


def library_class(img: Image) -> dict[int, tuple[str, str]]:
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
    # Fill: an unclassified function whose nearest classified neighbours on both sides (by address) share a family
    # and lie within 0x2000 bytes of each other belongs to that library's object-file run.
    entries = sorted(img.funcs)
    seeds = [e for e in entries if e in lib]
    filled = {}
    for e in entries:
        if e in lib:
            continue
        i = bisect.bisect_left(seeds, e)
        if 0 < i < len(seeds):
            a, b = seeds[i - 1], seeds[i]
            if lib[a][0] == lib[b][0] and b - a <= 0x2000:
                filled[e] = (lib[a][0], "address-run")
    lib.update(filled)
    return lib


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
    lib = library_class(img)

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
    q = collections.deque(sorted(roots))
    while q:
        a = q.popleft()
        nxt = set(img.callees.get(a, {}))
        if depth[a] == 0:
            nxt |= vt_peers.get(a, set())
        for b in sorted(nxt):
            if b not in depth and b in img.funcs and status_of.get(b) != "Library":
                depth[b] = depth[a] + 1
                root_area[b] = root_area[a]
                q.append(b)
    cand = []
    for e, d in depth.items():
        if d == 0 or status_of[e] == "Library" or e in by_func or img.funcs[e]["thunk"] == "1":
            continue
        size = img.funcs[e]["_size"]
        fanin = len(img.callers.get(e, {}))
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
        "rtti_families": dict(sorted(families.items(), key=lambda x: -x[1]["classes"])),
        "applied_names": len(applied),
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({k: summary[k] for k in ("functions", "status", "cited_pct_of_nonlib_bytes", "citations",
                                               "citing_files", "distinct_addresses_by_kind", "library_families")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
