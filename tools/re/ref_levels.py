#!/usr/bin/env python3
"""Generate docs/REFERENCE_LEVELS.md: the per-level comparison matrix (brief Phase 9, work item W9-1).

Inputs (all in the repository, nothing is measured here):
  docs/matrix/NN_*.md        chapter units, per-category implementation status, chapter self-tests
  docs/REFERENCE_COMPARISON.md  rows measured against tru.exe under Wine (status matches / differs / unmeasured)
  docs/audit/BUGS.md         open bug entries and the units they name
  tests/selftests/           chapter self-tests (chNN_*) and parked known gaps (known_gaps/chNN_*)

A REFERENCE_COMPARISON row is attributed to a chapter when its text names one of the chapter's units; a BUGS row
when its location names one. Rows naming no unit are general mechanics and are listed once, not per chapter.

Usage: tools/re/ref_levels.py [--check]   (--check: exit 1 if the committed file is out of date)
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UNIT = re.compile(r"\bl\d_[a-z0-9_]+[a-z0-9]")

# REFERENCE_COMPARISON section -> subsystem column
SECTION_SUBSYSTEM = {
    "Movement": "Locomotion",
    "Ledges and climbing": "Traversal",
    "Poles and beams": "Traversal",
    "Grapple": "Grapple",
    "Water": "Swimming / water",
    "Combat": "Combat / AI",
    "Objects, puzzles, hazards": "Objects / puzzles",
    "Motorcycle": "Vehicle",
    "Systems, camera": "Camera / systems",
}
SUBSYSTEMS = ["Locomotion", "Traversal", "Grapple", "Swimming / water", "Combat / AI", "Objects / puzzles",
              "Vehicle", "Camera / systems"]
# matrix category -> subsystem column (implementation status in our port)
CATEGORY_SUBSYSTEM = {
    "Movement": "Locomotion", "Traversal": "Traversal", "Grapple": "Grapple", "Swimming": "Swimming / water",
    "Combat": "Combat / AI", "Puzzles": "Objects / puzzles", "Vehicle": "Vehicle",
}
# BUGS subsystem prefix -> subsystem column
BUG_SUBSYSTEM = {
    "PLY": "Traversal", "SWM": "Swimming / water", "WAT": "Swimming / water", "GRP": "Grapple",
    "CMB": "Combat / AI", "PZL": "Objects / puzzles", "PHY": "Objects / puzzles", "VEH": "Vehicle",
    "CAM": "Camera / systems", "SCR": "Objects / puzzles", "REN": "Rendering", "AUD": "Audio",
}
CLOSED = re.compile(r"fixed|not a bug|not reproduced|not reproducible", re.I)


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def short_status(s):
    s = s.strip()
    if s.lower().startswith("matches"):
        return "matches"
    if s.lower().startswith("differs"):
        return "differs"
    if s.lower().startswith("unmeasured"):
        return "unmeasured"
    return s.split(";")[0][:40]


def impl_short(s):
    s = s.strip()
    for key, out in (("VERIFIED AGAINST ORIGINAL", "verified"), ("IMPLEMENTED BUT UNVERIFIED", "unverified"),
                     ("PARTIALLY IMPLEMENTED", "partial"), ("NOT IMPLEMENTED", "missing"),
                     ("IMPLEMENTED", "implemented")):
        if s.upper().startswith(key) or s.upper().startswith("(1) " + key):
            return out
    if s.lower().startswith("n/a"):
        return "n/a"
    return "see matrix"


def chapters():
    out = []
    for f in sorted((ROOT / "docs/matrix").glob("[0-9][0-9]_*.md")):
        text = f.read_text()
        lines = text.splitlines()
        title = lines[0].lstrip("# ").strip()
        num = int(f.name[:2])
        head = "\n".join(lines[:12])
        units = list(dict.fromkeys(UNIT.findall(head)))
        req_units = []
        cats = {}
        in_req = False
        for line in lines:
            if line.startswith("### Requirements"):
                in_req = True
            elif line.startswith("### "):
                in_req = False
            if not line.startswith("| "):
                continue
            c = cells(line)
            if not in_req and len(c) >= 4 and c[0] in CATEGORY_SUBSYSTEM | {"Chapter": 0} and c[0] not in cats:
                cats[c[0]] = c[2]
            if in_req and len(c) >= 5 and re.match(r"C\d\d-\d+", c[0]):
                req_units += UNIT.findall(c[3])
        if not units:  # no "Our units" line: the units the requirement rows name, in route order
            units = list(dict.fromkeys(req_units))
        out.append(dict(num=num, title=title, file=f.relative_to(ROOT).as_posix(), units=units, cats=cats))
    return out


def comparison_rows():
    rows, section = [], None
    for line in (ROOT / "docs/REFERENCE_COMPARISON.md").read_text().splitlines():
        if line.startswith("## "):
            name = line[3:].split(" (")[0].strip()
            section = SECTION_SUBSYSTEM.get(name)
            continue
        if section and line.startswith("| ") and not line.startswith("| ID") and not line.startswith("|---"):
            c = cells(line)
            if len(c) < 7:
                continue
            rows.append(dict(id=c[0], mech=c[1], sub=section, status=short_status(c[-1]),
                             units=set(UNIT.findall(line))))
    return rows


def bug_rows():
    rows = {}
    for line in (ROOT / "docs/audit/BUGS.md").read_text().splitlines():
        m = re.match(r"\| (OU-([A-Z]+)-\d+) \|", line)
        if not m:
            continue
        c = cells(line)
        bid, pre = m.group(1), m.group(2)
        r = rows.setdefault(bid, dict(id=bid, sub=BUG_SUBSYSTEM.get(pre, "Other"), title="", status="", units=set()))
        r["units"] |= set(UNIT.findall(line))
        if len(c) >= 6 and not r["title"]:
            r["title"], r["status"] = c[2], c[3]
    return [r for r in rows.values() if r["status"] and not CLOSED.search(r["status"])]


def selftests():
    by_ch, gaps = defaultdict(list), defaultdict(list)
    d = ROOT / "tests/selftests"
    for f in sorted(d.glob("ch[0-9][0-9]_*.json")):
        by_ch[int(f.name[2:4])].append(f.stem)
    for f in sorted((d / "known_gaps").glob("ch[0-9][0-9]_*.json")):
        gaps[int(f.name[2:4])].append(f.stem)
    return by_ch, gaps


def cell(sub, impl, comp, bugs):
    parts = []
    if impl:
        parts.append(impl)
    counts = defaultdict(int)
    for r in comp:
        counts[r["status"]] += 1
    if counts:
        parts.append(" / ".join(f"{n} {s}" for s, n in sorted(counts.items())))
    if bugs:
        parts.append(f"{len(bugs)} open")
    return "; ".join(parts) if parts else "—"


def render():
    chs = chapters()
    comp = comparison_rows()
    bugs = bug_rows()
    tests, gaps = selftests()
    attributed_c, attributed_b = set(), set()
    o = []
    o.append("# Per-level comparison: tru.exe against OpenUnderworld, chapter by chapter")
    o.append("")
    o.append("Generated by `tools/re/ref_levels.py` from [REFERENCE_COMPARISON.md](REFERENCE_COMPARISON.md) (rows measured "
             "against tru.exe under Wine), the chapter matrices in [matrix/](matrix/), [audit/BUGS.md](audit/BUGS.md) and "
             "`tests/selftests/`. Do not edit by hand: change the sources and re-run the script (`--check` fails when "
             "this file is stale). Brief Phase 9, work item W9-1.")
    o.append("")
    o.append("Each cell combines up to three facts, separated by `;`:")
    o.append("")
    o.append("- **our implementation** in that chapter, from the chapter matrix's category table: verified (against "
             "the original), implemented, unverified (implemented, not checked against the original), partial, "
             "missing, n/a;")
    o.append("- **measured comparison rows** from REFERENCE_COMPARISON whose evidence names one of the chapter's "
             "units, counted by status (`matches` / `differs` / `unmeasured`); a mechanic measured in another "
             "chapter is not counted here, see \"General mechanics\";")
    o.append("- **open bugs** from BUGS.md whose location names one of the chapter's units.")
    o.append("")
    o.append("Unit lists come from each matrix's \"Our units\" line, or the units its requirement rows name. "
             "Rendering and audio are compared per scene, not per chapter: see REFERENCE_COMPARISON and BUGS "
             "(OU-REN-*, OU-AUD-*).")
    o.append("")
    o.append("## Summary")
    o.append("")
    o.append("| Chapter | " + " | ".join(SUBSYSTEMS) + " | Self-tests (parked) |")
    o.append("|---|" + "---|" * len(SUBSYSTEMS) + "---|")
    detail = []
    for ch in chs:
        us = set(ch["units"])
        cc = [r for r in comp if r["units"] & us]
        bb = [b for b in bugs if b["units"] & us]
        attributed_c |= {id(r) for r in cc}
        attributed_b |= {b["id"] for b in bb}
        impl = {CATEGORY_SUBSYSTEM[k]: impl_short(v) for k, v in ch["cats"].items() if k in CATEGORY_SUBSYSTEM}
        row = []
        for s in SUBSYSTEMS:
            row.append(cell(s, impl.get(s), [r for r in cc if r["sub"] == s], [b for b in bb if b["sub"] == s]))
        anchor = re.sub(r"[^a-z0-9 -]", "", ch["title"].lower()).replace(" ", "-")
        o.append(f"| [{ch['num']:02d}](#{anchor}) | " + " | ".join(row) +
                 f" | {len(tests[ch['num']])} ({len(gaps[ch['num']])}) |")
        d = [f"## {ch['title']}", ""]
        d.append(f"Matrix: [{ch['file'].split('/')[-1]}]({ch['file'][5:]}). Units: "
                 + (", ".join(f"`{u}`" for u in ch["units"]) or "none named") + ".")
        d.append("")
        d.append("| Subsystem | Ours (matrix) | Measured against tru.exe | Open bugs |")
        d.append("|---|---|---|---|")
        for s in SUBSYSTEMS:
            rs = [r for r in cc if r["sub"] == s]
            bs = [b for b in bb if b["sub"] == s]
            meas = ", ".join(f"{r['id']} {r['mech']}: {r['status']}" for r in rs) or "unmeasured here"
            d.append(f"| {s} | {impl.get(s, '—')} | {meas} | {', '.join(b['id'] for b in bs) or '—'} |")
        other = [b for b in bb if b["sub"] not in SUBSYSTEMS]
        if other:
            d.append("")
            d.append("Other open bugs here: " + ", ".join(f"{b['id']} ({b['title']})" for b in other) + ".")
        d.append("")
        if tests[ch["num"]]:
            d.append("Self-tests: " + ", ".join(f"`{t}`" for t in tests[ch["num"]]) + ".")
        if gaps[ch["num"]]:
            d.append("Parked known gaps: " + ", ".join(f"`known_gaps/{t}`" for t in gaps[ch["num"]]) + ".")
        d.append("")
        detail += d
    o.append("")
    o += detail
    o.append("## General mechanics (measured, no chapter attributed)")
    o.append("")
    o.append("Rows of REFERENCE_COMPARISON whose evidence names no chapter unit (synthetic scenes, or a unit the "
             "chapter matrices do not list). They hold in every chapter that uses the mechanic.")
    o.append("")
    o.append("| Subsystem | Rows |")
    o.append("|---|---|")
    for s in SUBSYSTEMS:
        rs = [r for r in comp if r["sub"] == s and id(r) not in attributed_c]
        counts = defaultdict(int)
        for r in rs:
            counts[r["status"]] += 1
        o.append(f"| {s} | " + (" / ".join(f"{n} {k}" for k, n in sorted(counts.items())) or "—") + " |")
    o.append("")
    rest = [b for b in bugs if b["id"] not in attributed_b]
    o.append("Open bugs not tied to a chapter unit: " + ", ".join(b["id"] for b in sorted(rest, key=lambda b: b["id"]))
             + ".")
    o.append("")
    return "\n".join(o)


def main():
    out = ROOT / "docs/REFERENCE_LEVELS.md"
    text = render()
    if "--check" in sys.argv:
        sys.exit(0 if out.exists() and out.read_text() == text else 1)
    out.write_text(text)


if __name__ == "__main__":
    main()
