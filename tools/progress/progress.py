#!/usr/bin/env python3
"""Generate the OpenUnderworld progress site (GitHub Pages) from files in the repository.

    python3 tools/progress/progress.py OUTDIR [--history] [--repo PATH]

Reads only tracked repository files (no game data, no build) and writes into OUTDIR:
index.html, progress.json, progress.svg (chapter map + per-phase bars) and the badges
badge-brief.svg, badge-bugs.svg, badge-fidelity.svg, badge-chapters.svg, badge-re.svg.
With --history it also writes history.html and history.json, computed from every release
tag v* (`git cat-file` on the tag; metrics whose files are missing or have another
format at that tag are left empty).

Every parser checks the table header it expects and classifies every row; an unknown
header or status raises ParseError and the script exits non-zero, so the site never
shows silent zeros after a document changes format.
"""

from __future__ import annotations

import argparse
import csv
import html
import io
import json
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_URL = "https://github.com/sergen213/openunderworld"
BLOB = REPO_URL + "/blob/main/"
SITE_URL = "https://sergen213.github.io/openunderworld/"

BRIEF = "docs/audit/BRIEF_COMPLIANCE.md"
BUGS = "docs/audit/BUGS.md"
COMPARISON = "docs/REFERENCE_COMPARISON.md"
LEVELS = "docs/REFERENCE_LEVELS.md"
MATRIX_DIR = "docs/matrix"
SELFTEST_DIR = "tests/selftests"
KNOWN_GAPS_DIR = "tests/selftests/known_gaps"
FUNCTIONS = "docs/re/db/functions.tsv"
VERIFICATION = "docs/re/db/verification.tsv"
TESTS_DIR = "tests"
CHAPTERS = 21


class ParseError(Exception):
    """A document no longer has the format a parser expects."""


# --------------------------------------------------------------------------- sources


class WorkTree:
    """Files of a checkout on disk."""

    def __init__(self, root: Path):
        self.root = root

    def read(self, path: str) -> str | None:
        p = self.root / path
        return p.read_text(encoding="utf-8") if p.is_file() else None

    def list(self, directory: str, suffix: str) -> list[str]:
        d = self.root / directory
        if not d.is_dir():
            return []
        return sorted(f"{directory}/{p.name}" for p in d.iterdir() if p.is_file() and p.name.endswith(suffix))


class GitTree:
    """Files of one git revision, read through a shared `git cat-file --batch` process."""

    def __init__(self, root: Path, rev: str, batch: subprocess.Popen):
        self.root, self.rev, self.batch = root, rev, batch

    @staticmethod
    def open_batch(root: Path) -> subprocess.Popen:
        return subprocess.Popen(["git", "-C", str(root), "cat-file", "--batch"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def read(self, path: str) -> str | None:
        self.batch.stdin.write(f"{self.rev}:{path}\n".encode())
        self.batch.stdin.flush()
        header = self.batch.stdout.readline().decode().split()
        if len(header) < 3 or header[1] != "blob":
            return None
        data = self.batch.stdout.read(int(header[2]))
        self.batch.stdout.read(1)  # trailing newline
        return data.decode("utf-8", errors="replace")

    def list(self, directory: str, suffix: str) -> list[str]:
        out = subprocess.run(["git", "-C", str(self.root), "ls-tree", "--name-only", f"{self.rev}:{directory}"],
                             capture_output=True, text=True)
        if out.returncode != 0:
            return []
        return sorted(f"{directory}/{n}" for n in out.stdout.split("\n") if n.endswith(suffix))


# --------------------------------------------------------------------------- table helpers


def cells(line: str) -> list[str]:
    return [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]


def table_after(lines: list[str], start: int, header_ok, what: str) -> tuple[int, list[str], list[tuple[int, list[str]]]]:
    """Find the first table header at or after `start` accepted by header_ok; return
    (header line index, header cells, [(line index, row cells)])."""
    for i in range(start, len(lines)):
        if lines[i].startswith("|") and header_ok(cells(lines[i])):
            if i + 1 >= len(lines) or not lines[i + 1].startswith("|---"):
                raise ParseError(f"{what}: header at line {i + 1} has no separator row")
            head = cells(lines[i])
            rows = []
            j = i + 2
            while j < len(lines) and lines[j].startswith("|"):
                row = cells(lines[j])
                if len(row) != len(head):
                    raise ParseError(f"{what}: line {j + 1} has {len(row)} cells, header has {len(head)}")
                rows.append((j, row))
                j += 1
            if not rows:
                raise ParseError(f"{what}: table at line {i + 1} has no rows")
            return i, head, rows
    raise ParseError(f"{what}: table header not found")


def heading_index(lines: list[str], pattern: str, what: str) -> int:
    rx = re.compile(pattern)
    for i, line in enumerate(lines):
        if rx.match(line):
            return i
    raise ParseError(f"{what}: heading /{pattern}/ not found")


def plain(s: str) -> str:
    return s.replace("`", "").replace("*", "").strip()


# --------------------------------------------------------------------------- parsers

BRIEF_HEADER = ["Section", "Met", "Partly", "Missing (Not met)", "N/A"]


def parse_brief(text: str) -> dict:
    """§0 summary of BRIEF_COMPLIANCE.md: rows per section by status, plus the total row."""
    lines = text.splitlines()
    start = heading_index(lines, r"## 0\. Summary", "brief")
    hl, _, rows = table_after(lines, start, lambda h: h == BRIEF_HEADER, "brief §0")
    sections, total = [], None
    for ln, row in rows:
        name = plain(row[0])
        try:
            met, partly, missing, na = (int(plain(c)) for c in row[1:])
        except ValueError:
            raise ParseError(f"brief §0: non-numeric cell at line {ln + 1}") from None
        entry = {"section": name, "met": met, "partly": partly, "missing": missing, "na": na}
        if name.lower() == "total":
            total = entry
        else:
            sections.append(entry)
    if total is None:
        raise ParseError("brief §0: no Total row")
    for k in ("met", "partly", "missing", "na"):
        if sum(s[k] for s in sections) != total[k]:
            raise ParseError(f"brief §0: column {k} does not add up to the Total row")
    m = re.search(r"^Date: (\d{4}-\d{2}-\d{2})", text, re.M)
    applicable = total["met"] + total["partly"] + total["missing"]
    return {"sections": sections, "total": total, "line": hl + 1, "date": m.group(1) if m else None,
            "met_pct": 100.0 * total["met"] / applicable}


BUG_HEADER = ["ID", "Subsystem", "Title", "Status", "Origin", "Cluster"]
BUG_KINDS = ["fixed", "partly", "open", "reproduced", "not_reproduced", "not_a_bug"]
BUG_LABELS = {"fixed": "Fixed", "partly": "Partly fixed", "open": "Open", "reproduced": "Reproduced (open)",
              "not_reproduced": "Not reproduced", "not_a_bug": "Not a bug"}


def bug_kind(status: str) -> str:
    s = plain(status).lower()
    for prefix, kind in (("fixed", "fixed"), ("partly fixed", "partly"), ("open", "open"),
                         ("needs-repro", "open"), ("needs-windows-ref", "open"), ("reproduced", "reproduced"),
                         ("not reproduced", "not_reproduced"), ("not reproducible", "not_reproduced"),
                         ("not a bug", "not_a_bug")):
        if s.startswith(prefix):
            return kind
    raise ParseError(f"bugs: unknown status {status!r}")


def parse_bugs(text: str) -> dict:
    """Index table of BUGS.md: status per bug, by ID prefix (OU-PLY, OU-CAM, ...)."""
    lines = text.splitlines()
    start = heading_index(lines, r"## Index\s*$", "bugs")
    hl, _, rows = table_after(lines, start, lambda h: h == BUG_HEADER, "bugs index")
    counts, by_prefix = Counter(), {}
    for ln, row in rows:
        m = re.fullmatch(r"(OU-[A-Z0-9]+)-\d+", row[0])
        if not m:
            raise ParseError(f"bugs index: bad ID {row[0]!r} at line {ln + 1}")
        kind = bug_kind(row[3])
        counts[kind] += 1
        by_prefix.setdefault(m.group(1), Counter())[kind] += 1
    total = sum(counts.values())
    still_open = counts["open"] + counts["reproduced"] + counts["partly"]
    return {"counts": {k: counts[k] for k in BUG_KINDS}, "total": total, "open_total": still_open,
            "by_prefix": {p: {k: c[k] for k in BUG_KINDS} for p, c in sorted(by_prefix.items())},
            "line": hl + 1, "fixed_pct": 100.0 * counts["fixed"] / total}


CMP_KINDS = ["match", "match_part_differs", "differs", "unmeasured"]
CMP_LABELS = {"match": "Matches", "match_part_differs": "Matches, one part differs", "differs": "Differs",
              "unmeasured": "Unmeasured"}


def cmp_kind(status: str) -> str:
    s = plain(status).lower()
    if s.startswith("unmeasured"):
        return "unmeasured"
    if s.startswith("matches"):
        return "match_part_differs" if "differs" in s else "match"
    if s.startswith("differs"):
        return "differs"
    raise ParseError(f"comparison: unknown status {status[:80]!r}")


def parse_comparison(text: str) -> dict:
    """Every mechanic table of REFERENCE_COMPARISON.md (header `| ID | Mechanic | ... | Status |`)."""
    lines = text.splitlines()
    counts, sections = Counter(), []
    pos, heading = 0, None
    found = 0
    while True:
        try:
            hl, head, rows = table_after(lines, pos, lambda h: h[:2] == ["ID", "Mechanic"], "comparison")
        except ParseError as e:
            if found and "not found" in str(e):
                break
            raise
        found += 1
        if head[-1] != "Status":
            raise ParseError(f"comparison: table at line {hl + 1} has no trailing Status column")
        for k in range(hl, -1, -1):
            if lines[k].startswith("## "):
                heading = re.sub(r"\s*\(.*\)\s*$", "", lines[k][3:]).strip()
                break
        sec = Counter(cmp_kind(row[-1]) for _, row in rows)
        counts.update(sec)
        sections.append({"section": heading, "line": hl + 1, **{k: sec[k] for k in CMP_KINDS}})
        pos = rows[-1][0] + 1
    total = sum(counts.values())
    measured = total - counts["unmeasured"]
    matching = counts["match"] + counts["match_part_differs"]
    return {"counts": {k: counts[k] for k in CMP_KINDS}, "total": total, "measured": measured,
            "matching": matching, "sections": sections,
            "measured_pct": 100.0 * measured / total, "match_pct_of_measured": 100.0 * matching / measured}


REQ_KINDS = ["verified", "implemented", "unverified", "partial", "missing", "na"]
REQ_LABELS = {"verified": "Verified against original", "implemented": "Implemented",
              "unverified": "Implemented, unverified", "partial": "Partially implemented",
              "missing": "Not implemented", "na": "n/a"}


def req_kind(status: str) -> str:
    s = plain(status)
    for prefix, kind in (("VERIFIED AGAINST ORIGINAL", "verified"), ("IMPLEMENTED BUT UNVERIFIED", "unverified"),
                         ("PARTIALLY IMPLEMENTED", "partial"), ("NOT IMPLEMENTED", "missing"),
                         ("IMPLEMENTED", "implemented")):
        if s.startswith(prefix):
            return kind
    if s.lower().startswith("n/a"):
        return "na"
    raise ParseError(f"matrix: unknown requirement status {status[:80]!r}")


def parse_matrix(text: str) -> dict:
    """One docs/matrix/NN_*.md: chapter title and the Requirements table's status column."""
    lines = text.splitlines()
    m = re.match(r"## (\d\d)\. (.+?)(?: \((.+)\))?$", lines[0] if lines else "")
    if not m:
        raise ParseError("matrix: first line is not '## NN. Title (Place)'")
    hl, head, rows = table_after(lines, 0, lambda h: h[:2] == ["ID", "Where in the chapter (own words, in route order)"],
                                 "matrix requirements")
    if "Status" not in head:
        raise ParseError("matrix: requirements table has no Status column")
    si = head.index("Status")
    counts = Counter(req_kind(row[si]) for _, row in rows)
    applicable = sum(counts.values()) - counts["na"]
    done = counts["verified"] + counts["implemented"] + counts["unverified"]
    return {"chapter": int(m.group(1)), "title": m.group(2), "place": m.group(3) or "",
            "counts": {k: counts[k] for k in REQ_KINDS}, "rows": sum(counts.values()), "applicable": applicable,
            "implemented": done, "line": hl + 1, "implemented_pct": 100.0 * done / applicable if applicable else 0.0}


def parse_levels(text: str) -> dict:
    """Summary table of REFERENCE_LEVELS.md: self-tests per chapter and how many are parked."""
    lines = text.splitlines()
    start = heading_index(lines, r"## Summary\s*$", "levels")
    hl, head, rows = table_after(lines, start, lambda h: h[:1] == ["Chapter"], "levels summary")
    if head[-1] != "Self-tests (parked)":
        raise ParseError("levels: last column is not 'Self-tests (parked)'")
    out = {}
    for ln, row in rows:
        cm = re.match(r"\[(\d\d)\]", row[0])
        sm = re.fullmatch(r"(\d+) \((\d+)\)", row[-1])
        if not cm or not sm:
            raise ParseError(f"levels: unexpected row at line {ln + 1}")
        out[int(cm.group(1))] = {"selftests": int(sm.group(1)), "parked": int(sm.group(2))}
    return {"chapters": out, "line": hl + 1}


FUNC_STATUSES = ["Reimplemented-cited", "Named-only", "Library", "Unknown"]
FUNC_LABELS = {"Reimplemented-cited": "Reimplemented, cited in the repo", "Named-only": "Named, not reimplemented",
               "Library": "Library (CRT, FMOD, bfx)", "Unknown": "Unknown"}


def read_tsv(text: str, required: list[str], what: str) -> list[dict]:
    reader = csv.DictReader(io.StringIO(text), delimiter="\t")
    missing = [c for c in required if c not in (reader.fieldnames or [])]
    if missing:
        raise ParseError(f"{what}: missing columns {missing}")
    rows = list(reader)
    if not rows:
        raise ParseError(f"{what}: no rows")
    return rows


def parse_functions(text: str) -> dict:
    """docs/re/db/functions.tsv: every tru.exe function with its status and confidence."""
    rows = read_tsv(text, ["entry", "size", "status", "confidence"], "functions.tsv")
    counts, sizes, conf = Counter(), Counter(), Counter()
    for r in rows:
        if r["status"] not in FUNC_STATUSES:
            raise ParseError(f"functions.tsv: unknown status {r['status']!r} at {r['entry']}")
        counts[r["status"]] += 1
        sizes[r["status"]] += int(r["size"] or 0)
        conf[r["confidence"]] += 1
    total = len(rows)
    identified = total - counts["Unknown"]
    nonlib_bytes = sum(sizes.values()) - sizes["Library"]
    return {"total": total, "identified": identified, "counts": {k: counts[k] for k in FUNC_STATUSES},
            "bytes": {k: sizes[k] for k in FUNC_STATUSES}, "confidence": dict(sorted(conf.items())),
            "identified_pct": 100.0 * identified / total,
            "cited_bytes_pct_of_nonlib": 100.0 * sizes["Reimplemented-cited"] / nonlib_bytes}


VERDICTS = ["match", "partially", "mismatch", "not-found-in-function"]


def parse_verification(text: str) -> dict:
    """docs/re/db/verification.tsv: instruction-level checks of repo claims against tru.exe."""
    rows = read_tsv(text, ["id", "verdict", "confidence"], "verification.tsv")
    counts = Counter()
    for r in rows:
        if r["verdict"] not in VERDICTS:
            raise ParseError(f"verification.tsv: unknown verdict {r['verdict']!r} in {r['id']}")
        counts[r["verdict"]] += 1
    return {"total": len(rows), "counts": {k: counts[k] for k in VERDICTS}}


TEST_RX = re.compile(r"^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST)\s*\(", re.M)


def count_tests(src) -> dict:
    """gtest TEST/TEST_F/TEST_P/TYPED_TEST macros in tests/*.cpp and self-test JSON scenarios.
    (ctest registrations need a configured build; these are the source-level counts.)"""
    files = src.list(TESTS_DIR, ".cpp")
    per_file = {f.split("/")[-1]: len(TEST_RX.findall(src.read(f) or "")) for f in files}
    gtest = sum(per_file.values())
    if gtest == 0:
        raise ParseError("tests: no gtest macros found in tests/*.cpp")
    scenarios = src.list(SELFTEST_DIR, ".json")
    gaps = src.list(KNOWN_GAPS_DIR, ".json")
    return {"gtest_macros": gtest, "gtest_files": len(files), "per_file": per_file,
            "selftest_scenarios": len(scenarios), "known_gaps": [g.split("/")[-1] for g in gaps]}


# --------------------------------------------------------------------------- collection


def chapter_bucket(ch: dict) -> str:
    if ch["blocked"]:
        return "blocked"
    if ch["counts"]["partial"] == 0 and ch["counts"]["missing"] == 0:
        return "complete"
    return "mostly" if ch["implemented_pct"] >= 80.0 else "partial"


BUCKETS = {"complete": ("#1f883d", "every requirement implemented"),
           "mostly": ("#5f8f1a", "80 % or more implemented"),
           "partial": ("#b7791f", "under 80 % implemented"),
           "blocked": ("#c62828", "route blocked: a parked self-test (known gap)")}


def collect_chapters(src, strict: bool) -> dict:
    files = src.list(MATRIX_DIR, ".md")
    matrices = {}
    for f in files:
        if not re.match(r".*/\d\d_.*\.md$", f):
            continue
        m = parse_matrix(src.read(f))
        m["file"] = f
        matrices[m["chapter"]] = m
    if strict and sorted(matrices) != list(range(1, CHAPTERS + 1)):
        raise ParseError(f"matrix: expected chapters 1..{CHAPTERS}, found {sorted(matrices)}")
    if not matrices:
        raise ParseError("matrix: no chapter files")
    levels_text = src.read(LEVELS)
    levels = parse_levels(levels_text) if levels_text is not None else None
    if strict and (levels is None or sorted(levels["chapters"]) != sorted(matrices)):
        raise ParseError("levels: summary chapters do not match the matrices")
    gaps = [g.split("/")[-1] for g in src.list(KNOWN_GAPS_DIR, ".json")]
    chapters = []
    for n, m in sorted(matrices.items()):
        lv = (levels or {"chapters": {}})["chapters"].get(n, {"selftests": None, "parked": 0})
        gap_files = [g for g in gaps if g.startswith(f"ch{n:02d}_")]
        ch = dict(m, selftests=lv["selftests"], parked=lv["parked"], known_gaps=gap_files)
        ch["blocked"] = bool(lv["parked"] or gap_files)
        ch["bucket"] = chapter_bucket(ch)
        chapters.append(ch)
    totals = Counter()
    for ch in chapters:
        totals.update(ch["counts"])
    applicable = sum(totals.values()) - totals["na"]
    done = totals["verified"] + totals["implemented"] + totals["unverified"]
    return {"chapters": chapters, "counts": {k: totals[k] for k in REQ_KINDS}, "applicable": applicable,
            "implemented": done, "implemented_pct": 100.0 * done / applicable,
            "blocked": sum(1 for c in chapters if c["blocked"]), "count": len(chapters),
            "levels_line": levels["line"] if levels else None}


def collect(src, strict: bool = True) -> dict:
    """All metrics. strict=True (current tree): a missing file or format change raises.
    strict=False (history): metrics whose files are missing or unparsable become None."""

    def metric(fn, path):
        text = src.read(path)
        if text is None:
            if strict:
                raise ParseError(f"{path}: file missing")
            return None
        try:
            return fn(text)
        except ParseError as e:
            if strict:
                raise ParseError(f"{path}: {e}") from None
            return None

    def guarded(fn):
        try:
            return fn()
        except ParseError:
            if strict:
                raise
            return None

    return {
        "brief": metric(parse_brief, BRIEF),
        "bugs": metric(parse_bugs, BUGS),
        "comparison": metric(parse_comparison, COMPARISON),
        "chapters": guarded(lambda: collect_chapters(src, strict)),
        "re": metric(parse_functions, FUNCTIONS),
        "verification": metric(parse_verification, VERIFICATION),
        "tests": guarded(lambda: count_tests(src)),
    }


# --------------------------------------------------------------------------- rendering helpers


def pct(x: float) -> str:
    return f"{int(x)}%"  # truncated: never round a figure up


def esc(s) -> str:
    return html.escape(str(s), quote=True)


def src_link(path: str, line: int | None = None) -> str:
    return BLOB + path + (f"?plain=1#L{line}" if line else "")


def a(text, path: str, line: int | None = None) -> str:
    return f'<a href="{esc(src_link(path, line))}">{esc(text)}</a>'


def text_width(s: str) -> float:
    """Approximate Verdana 11 px advance; textLength makes the renderer fit exactly."""
    w = 0.0
    for c in s:
        if c in "il.,:;|!'1 ":
            w += 3.8
        elif c in "mwMW%":
            w += 10.0
        elif c.isupper() or c.isdigit():
            w += 7.4
        else:
            w += 6.6
    return w


def badge(label: str, message: str, color: str) -> str:
    lw, mw = text_width(label) + 12, text_width(message) + 12
    w = lw + mw
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="20" role="img" aria-label="{esc(label)}: {esc(message)}">
<title>{esc(label)}: {esc(message)}</title>
<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient>
<clipPath id="r"><rect width="{w:.0f}" height="20" rx="3" fill="#fff"/></clipPath>
<g clip-path="url(#r)"><rect width="{lw:.0f}" height="20" fill="#555"/><rect x="{lw:.0f}" width="{mw:.0f}" height="20" fill="{color}"/><rect width="{w:.0f}" height="20" fill="url(#s)"/></g>
<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">
<text x="{lw / 2:.1f}" y="15" fill="#010101" fill-opacity=".3" textLength="{lw - 12:.0f}">{esc(label)}</text><text x="{lw / 2:.1f}" y="14" textLength="{lw - 12:.0f}">{esc(label)}</text>
<text x="{lw + mw / 2:.1f}" y="15" fill="#010101" fill-opacity=".3" textLength="{mw - 12:.0f}">{esc(message)}</text><text x="{lw + mw / 2:.1f}" y="14" textLength="{mw - 12:.0f}">{esc(message)}</text>
</g></svg>
"""


def scale_color(p: float) -> str:
    for limit, color in ((90, "#2ea043"), (75, "#7cb518"), (50, "#dfb317"), (25, "#fe7d37")):
        if p >= limit:
            return color
    return "#e05d44"


def badges(d: dict) -> dict[str, str]:
    b, bugs, c, ch, r = d["brief"], d["bugs"], d["comparison"], d["chapters"], d["re"]
    unblocked = ch["count"] - ch["blocked"]
    return {
        "badge-brief.svg": badge("brief", f"{pct(b['met_pct'])} met", scale_color(b["met_pct"])),
        "badge-bugs.svg": badge("bugs", f"{bugs['counts']['fixed']} fixed, {bugs['open_total']} open",
                                scale_color(bugs["fixed_pct"])),
        "badge-fidelity.svg": badge("vs tru.exe", f"{c['matching']}/{c['measured']} measured match, "
                                                  f"{c['counts']['unmeasured']} unmeasured",
                                    scale_color(c["measured_pct"])),
        "badge-chapters.svg": badge("chapters", f"{unblocked}/{ch['count']} unblocked, "
                                                f"{pct(ch['implemented_pct'])} reqs implemented",
                                    scale_color(100.0 * unblocked / ch["count"])),
        "badge-re.svg": badge("tru.exe RE", f"{pct(r['identified_pct'])} of {r['total']} functions identified",
                              scale_color(r["identified_pct"])),
    }


PHASE_COLORS = {"met": "#2ea043", "partly": "#d29922", "missing": "#da3633", "na": "#8b949e"}


def progress_svg(d: dict, meta: dict) -> str:
    ch, b = d["chapters"], d["brief"]
    cols, tw, th, gap, x0 = 7, 128, 78, 6, 16
    rows = (len(ch["chapters"]) + cols - 1) // cols
    grid_y = 52
    out = []
    for i, c in enumerate(ch["chapters"]):
        x = x0 + (i % cols) * (tw + gap)
        y = grid_y + (i // cols) * (th + gap)
        color = BUCKETS[c["bucket"]][0]
        name = c["title"]
        while text_width(name) * 1.08 > tw - 14:  # bold 12.5 px against the 11 px estimate
            name = name[:-2].rstrip() + "…"
        third = "BLOCKED" if c["blocked"] else f"{c['counts']['verified']} verified vs tru.exe"
        out.append(
            f'<g><title>{esc(c["title"])} ({esc(c["place"])}): {c["implemented"]}/{c["applicable"]} requirements '
            f'implemented, {c["counts"]["partial"]} partial, {c["counts"]["missing"]} missing; '
            f'{c["counts"]["verified"]} verified against tru.exe; self-tests {c["selftests"]}, parked {c["parked"]}</title>'
            f'<rect x="{x}" y="{y}" width="{tw}" height="{th}" rx="6" fill="{color}"/>'
            f'<text x="{x + 8}" y="{y + 20}" class="tn">{c["chapter"]:02d}</text>'
            f'<text x="{x + 36}" y="{y + 20}" class="tp">{pct(c["implemented_pct"])}</text>'
            f'<text x="{x + 8}" y="{y + 40}" class="tt">{esc(name)}</text>'
            f'<text x="{x + 8}" y="{y + 56}" class="ts">{c["implemented"]}/{c["applicable"]} reqs implemented</text>'
            f'<text x="{x + 8}" y="{y + 70}" class="ts">{esc(third)}</text></g>')
    ly = grid_y + rows * (th + gap) + 10
    legend = []
    lx = x0
    for key, (color, label) in BUCKETS.items():
        legend.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" rx="2" fill="{color}"/>'
                      f'<text x="{lx + 17}" y="{ly + 10}" class="lg">{esc(label)}</text>')
        lx += 17 + text_width(label) * 0.95 + 22
    by = ly + 44
    bars = [f'<text x="{x0}" y="{by - 12}" class="h2">Master brief compliance per phase '
            f'(Met {b["total"]["met"]} / Partly {b["total"]["partly"]} / Missing {b["total"]["missing"]} / '
            f'N/A {b["total"]["na"]})</text>']
    label_w, bar_w, bh = 270, 560, 16
    for i, s in enumerate(b["sections"]):
        y = by + i * (bh + 5)
        n = s["met"] + s["partly"] + s["missing"] + s["na"]
        bars.append(f'<text x="{x0}" y="{y + 12}" class="bl">{esc(s["section"])}</text>')
        x = x0 + label_w
        for k in ("met", "partly", "missing", "na"):
            if s[k]:
                w = bar_w * s[k] / n
                bars.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{bh}" fill="{PHASE_COLORS[k]}">'
                            f'<title>{esc(s["section"])}: {k} {s[k]}</title></rect>')
                x += w
        bars.append(f'<text x="{x0 + label_w + bar_w + 8}" y="{y + 12}" class="bl">'
                    f'{s["met"]}/{n} met</text>')
    ky = by + len(b["sections"]) * (bh + 5) + 8
    kx = x0 + label_w
    for k, label in (("met", "Met"), ("partly", "Partly"), ("missing", "Missing"), ("na", "N/A")):
        bars.append(f'<rect x="{kx}" y="{ky}" width="12" height="12" rx="2" fill="{PHASE_COLORS[k]}"/>'
                    f'<text x="{kx + 17}" y="{ky + 10}" class="lg">{label}</text>')
        kx += 90
    height = ky + 30
    width = x0 * 2 + cols * tw + (cols - 1) * gap
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-label="OpenUnderworld progress map">
<style>
.bg{{fill:#ffffff}} text{{font-family:-apple-system,"Segoe UI",Helvetica,Arial,"DejaVu Sans",sans-serif;fill:#1f2328}}
.h1{{font-size:18px;font-weight:700}} .h2{{font-size:14px;font-weight:600}} .sub,.lg,.bl{{font-size:12px}} .sub{{fill:#59636e}}
.tn,.tp,.tt,.ts{{fill:#ffffff}} .tn{{font-size:16px;font-weight:700}} .tp{{font-size:13px;font-weight:600}} .tt{{font-size:12.5px;font-weight:600}} .ts{{font-size:10.5px}}
@media (prefers-color-scheme: dark) {{ .bg{{fill:#0d1117}} text{{fill:#e6edf3}} .sub{{fill:#9198a1}} .tn,.tp,.tt,.ts{{fill:#ffffff}} }}
</style>
<rect class="bg" width="{width}" height="{height}" rx="8"/>
<text x="{x0}" y="24" class="h1">OpenUnderworld: chapters by implementation of their requirements</text>
<text x="{x0}" y="42" class="sub">Data {esc(meta["date"][:10])}, commit {esc(meta["sha"][:10])}. Percent = requirement rows implemented (docs/matrix), blocked = parked self-test.</text>
{"".join(out)}
{"".join(legend)}
{"".join(bars)}
</svg>
"""


CSS = """
:root{--bg:#fff;--fg:#1f2328;--muted:#59636e;--line:#d1d9e0;--card:#f6f8fa;--link:#0969da}
@media (prefers-color-scheme: dark){:root{--bg:#0d1117;--fg:#e6edf3;--muted:#9198a1;--line:#3d444d;--card:#151b23;--link:#4493f8}}
*{box-sizing:border-box} body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,"Segoe UI",Helvetica,Arial,sans-serif}
main{max-width:1060px;margin:0 auto;padding:24px 16px 48px} a{color:var(--link)} h1{margin:.2em 0} h2{margin-top:2em;border-bottom:1px solid var(--line);padding-bottom:.3em}
.muted{color:var(--muted)} .cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px;margin:20px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.card .n{font-size:28px;font-weight:700} .card .n a{text-decoration:none;color:inherit} .card .l{color:var(--muted);font-size:13px}
table{border-collapse:collapse;width:100%;margin:12px 0;font-size:14px} th,td{border:1px solid var(--line);padding:4px 8px;text-align:right}
th:first-child,td:first-child{text-align:left} th{background:var(--card)} td.l{text-align:left}
.bar{display:flex;height:12px;min-width:160px;border-radius:3px;overflow:hidden;background:var(--line)} .bar span{display:block;height:100%}
img.map{max-width:100%;height:auto;border-radius:8px} .badges img{margin:0 6px 6px 0}
"""


def page(title: str, body: str) -> str:
    return (f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<title>{esc(title)}</title><style>{CSS}</style></head><body><main>{body}</main></body></html>\n')


def stacked(parts: list[tuple[int, str, str]]) -> str:
    total = sum(n for n, _, _ in parts) or 1
    return '<div class="bar">' + "".join(
        f'<span style="width:{100.0 * n / total:.2f}%;background:{c}" title="{esc(t)}: {n}"></span>'
        for n, c, t in parts if n) + "</div>"


def header_html(meta: dict, title: str) -> str:
    return (f'<h1>{esc(title)}</h1><p class="muted">Data as of commit '
            f'<a href="{REPO_URL}/commit/{esc(meta["sha"])}"><code>{esc(meta["sha"][:10])}</code></a> '
            f'({esc(meta["date"])}), generated by {a("tools/progress/progress.py", "tools/progress/progress.py")} '
            f'from the repository\'s own documents. Every number links to the file it is counted from. '
            f'<a href="{REPO_URL}">Repository</a>.</p>')


def index_html(d: dict, meta: dict, has_history: bool) -> str:
    b, bugs, c, ch, r, v, t = (d[k] for k in ("brief", "bugs", "comparison", "chapters", "re", "verification", "tests"))
    nav = '<p><a href="history.html">Progress history per release</a> · <a href="progress.json">progress.json</a></p>' \
        if has_history else '<p><a href="progress.json">progress.json</a></p>'
    unblocked = ch["count"] - ch["blocked"]
    cards = [
        (a(pct(b["met_pct"]), BRIEF, b["line"]), f"master brief requirements met ({b['total']['met']} of "
                                                f"{b['total']['met'] + b['total']['partly'] + b['total']['missing']})"),
        (a(f"{bugs['counts']['fixed']}/{bugs['total']}", BUGS, bugs["line"]),
         f"bugs fixed; {bugs['open_total']} still open or partly fixed"),
        (a(f"{c['matching']}/{c['measured']}", COMPARISON), f"measured mechanics match tru.exe; "
                                                          f"{c['counts']['unmeasured']} of {c['total']} unmeasured"),
        (a(f"{unblocked}/{ch['count']}", LEVELS, ch["levels_line"]), "chapters without a parked (blocked) self-test"),
        (a(pct(ch["implemented_pct"]), MATRIX_DIR), f"chapter requirements implemented ({ch['implemented']} of "
                                                   f"{ch['applicable']}; {ch['counts']['verified']} verified vs tru.exe)"),
        (a(pct(r["identified_pct"]), FUNCTIONS), f"of {r['total']} tru.exe functions identified "
                                                f"({r['counts']['Reimplemented-cited']} reimplemented and cited)"),
        (a(f"{v['counts']['match']}/{v['total']}", VERIFICATION), "instruction-level claim checks that match tru.exe"),
        (a(t["gtest_macros"], TESTS_DIR), f"gtest TEST macros in {t['gtest_files']} tests/*.cpp files"),
        (a(t["selftest_scenarios"], SELFTEST_DIR), f"self-test JSON scenarios (+{len(t['known_gaps'])} parked)"),
    ]
    body = [header_html(meta, "OpenUnderworld progress"), nav,
            '<p class="badges">' + "".join(f'<img src="{n}" alt="{n}">' for n in badges(d)) + "</p>",
            '<div class="cards">' + "".join(f'<div class="card"><div class="n">{n}</div><div class="l">{esc(l)}</div></div>'
                                            for n, l in cards) + "</div>",
            '<img class="map" src="progress.svg" alt="Chapter map and per-phase brief compliance">']

    body.append(f'<h2>Master brief compliance</h2><p>Rows per section by status, from §0 of '
                f'{a("BRIEF_COMPLIANCE.md", BRIEF, b["line"])} (dated {esc(b["date"])}). Met share = Met / (Met + Partly + Missing).</p>'
                '<table><tr><th>Section</th><th>Met</th><th>Partly</th><th>Missing</th><th>N/A</th><th></th></tr>')
    for s in b["sections"] + [dict(b["total"], section="Total")]:
        body.append(f'<tr><td>{esc(s["section"])}</td><td>{s["met"]}</td><td>{s["partly"]}</td><td>{s["missing"]}</td>'
                    f'<td>{s["na"]}</td><td>{stacked([(s[k], PHASE_COLORS[k], k) for k in ("met", "partly", "missing", "na")])}</td></tr>')
    body.append("</table>")

    body.append(f'<h2>Chapters</h2><p>Requirement rows of each chapter matrix ({a("docs/matrix/", MATRIX_DIR)}) by status; '
                f'self-tests and parked (known-gap) scenarios from {a("REFERENCE_LEVELS.md", LEVELS, ch["levels_line"])} and '
                f'{a("tests/selftests/known_gaps/", KNOWN_GAPS_DIR)}. Colours: '
                + ", ".join(f'<span style="color:{col}">■</span> {esc(lbl)}' for col, lbl in BUCKETS.values())
                + '.</p><table><tr><th>Chapter</th><th>Verified</th><th>Implemented</th><th>Unverified</th><th>Partial</th>'
                  '<th>Missing</th><th>n/a</th><th>Implemented</th><th>Self-tests</th><th>Parked</th></tr>')
    for x in ch["chapters"]:
        k = x["counts"]
        gaps = ", ".join(x["known_gaps"])
        label = f"{x['chapter']:02d} {x['title']}"
        body.append(f'<tr><td><span style="color:{BUCKETS[x["bucket"]][0]}">■</span> '
                    f'{a(label, x["file"], x["line"])}</td>'
                    f'<td>{k["verified"]}</td><td>{k["implemented"]}</td><td>{k["unverified"]}</td><td>{k["partial"]}</td>'
                    f'<td>{k["missing"]}</td><td>{k["na"]}</td><td>{pct(x["implemented_pct"])}</td>'
                    f'<td>{x["selftests"] if x["selftests"] is not None else "—"}</td>'
                    f'<td title="{esc(gaps)}">{x["parked"]}</td></tr>')
    k = ch["counts"]
    body.append(f'<tr><th>Total</th><th>{k["verified"]}</th><th>{k["implemented"]}</th><th>{k["unverified"]}</th>'
                f'<th>{k["partial"]}</th><th>{k["missing"]}</th><th>{k["na"]}</th><th>{pct(ch["implemented_pct"])}</th>'
                f'<th></th><th>{ch["blocked"]} blocked</th></tr></table>')

    bug_colors = {"fixed": "#2ea043", "partly": "#d29922", "open": "#da3633", "reproduced": "#a40e26",
                  "not_reproduced": "#8b949e", "not_a_bug": "#57606a"}
    body.append(f'<h2>Bugs</h2><p>Status column of the index in {a("BUGS.md", BUGS, bugs["line"])}, by ID prefix. '
                f'"Fixed" includes fixes on integration branches not yet released.</p><table><tr><th>Prefix</th>'
                + "".join(f"<th>{esc(BUG_LABELS[k])}</th>" for k in BUG_KINDS) + "<th>Total</th><th></th></tr>")
    for p, cnt in list(bugs["by_prefix"].items()) + [("Total", bugs["counts"])]:
        body.append(f"<tr><td>{esc(p)}</td>" + "".join(f"<td>{cnt[k]}</td>" for k in BUG_KINDS)
                    + f"<td>{sum(cnt.values())}</td><td>{stacked([(cnt[k], bug_colors[k], BUG_LABELS[k]) for k in BUG_KINDS])}</td></tr>")
    body.append("</table>")

    cmp_colors = {"match": "#2ea043", "match_part_differs": "#7cb518", "differs": "#da3633", "unmeasured": "#8b949e"}
    body.append(f'<h2>Fidelity: measured against tru.exe</h2><p>Status column of every mechanic table in '
                f'{a("REFERENCE_COMPARISON.md", COMPARISON)}: rows measured on both sides (tru.exe under Wine and ours) '
                f'and whether they match within the document\'s tolerance.</p><table><tr><th>Section</th>'
                + "".join(f"<th>{esc(CMP_LABELS[k])}</th>" for k in CMP_KINDS) + "<th></th></tr>")
    for s in c["sections"] + [dict(c["counts"], section="Total", line=None)]:
        name = a(s["section"], COMPARISON, s["line"]) if s["line"] else "Total"
        body.append(f"<tr><td>{name}</td>" + "".join(f"<td>{s[k]}</td>" for k in CMP_KINDS)
                    + f"<td>{stacked([(s[k], cmp_colors[k], CMP_LABELS[k]) for k in CMP_KINDS])}</td></tr>")
    body.append("</table>")

    body.append(f'<h2>Reverse engineering of tru.exe</h2><p>Functions of tru.exe 1.1.0.0 in '
                f'{a("functions.tsv", FUNCTIONS)} by status; instruction-level verification of repo claims in '
                f'{a("verification.tsv", VERIFICATION)}.</p><table><tr><th>Status</th><th>Functions</th><th>Bytes</th></tr>')
    for k in FUNC_STATUSES:
        body.append(f'<tr><td>{esc(FUNC_LABELS[k])}</td><td>{r["counts"][k]}</td><td>{r["bytes"][k]}</td></tr>')
    body.append(f'<tr><th>Total</th><th>{r["total"]}</th><th>{sum(r["bytes"].values())}</th></tr></table>'
                f'<p>Reimplemented-and-cited code covers {r["cited_bytes_pct_of_nonlib"]:.1f} % of the non-library bytes. '
                f'Confidence: ' + ", ".join(f"{esc(k)} {n}" for k, n in r["confidence"].items()) + ".</p>"
                '<table><tr><th>Verification verdict</th><th>Claims</th></tr>'
                + "".join(f"<tr><td>{esc(k)}</td><td>{n}</td></tr>" for k, n in v["counts"].items())
                + f'<tr><th>Total</th><th>{v["total"]}</th></tr></table>')

    body.append(f'<h2>Tests</h2><p>Source-level counts (the ctest registration count needs a configured build): '
                f'gtest <code>TEST</code> / <code>TEST_F</code> / <code>TEST_P</code> macros per file in '
                f'{a("tests/", TESTS_DIR)}, and self-test scenarios in {a("tests/selftests/", SELFTEST_DIR)} '
                f'(parked: {esc(", ".join(t["known_gaps"]))}).</p><table><tr><th>File</th><th>gtest macros</th></tr>'
                + "".join(f'<tr><td>{a(f, f"{TESTS_DIR}/{f}")}</td><td>{n}</td></tr>' for f, n in t["per_file"].items())
                + f'<tr><th>Total</th><th>{t["gtest_macros"]}</th></tr></table>')
    return page("OpenUnderworld progress", "".join(body))


# --------------------------------------------------------------------------- history

HISTORY_SERIES = [
    ("brief", "Brief met %", "#2ea043", lambda d: d["brief"]["met_pct"] if d["brief"] else None),
    ("bugs", "Bugs fixed %", "#8250df", lambda d: d["bugs"]["fixed_pct"] if d["bugs"] else None),
    ("measured", "Comparison rows measured %", "#0969da",
     lambda d: d["comparison"]["measured_pct"] if d["comparison"] else None),
    ("chapters", "Chapter requirements implemented %", "#bf8700",
     lambda d: d["chapters"]["implemented_pct"] if d["chapters"] else None),
    ("re", "tru.exe functions identified %", "#cf222e", lambda d: d["re"]["identified_pct"] if d["re"] else None),
]


def summarize(d: dict) -> dict:
    """The flat per-revision numbers of history.json."""
    out = {key: (round(fn(d), 2) if fn(d) is not None else None) for key, _, _, fn in HISTORY_SERIES}
    out["bugs_fixed"] = d["bugs"]["counts"]["fixed"] if d["bugs"] else None
    out["bugs_total"] = d["bugs"]["total"] if d["bugs"] else None
    out["cmp_matching"] = d["comparison"]["matching"] if d["comparison"] else None
    out["cmp_measured"] = d["comparison"]["measured"] if d["comparison"] else None
    out["cmp_total"] = d["comparison"]["total"] if d["comparison"] else None
    out["chapters_blocked"] = d["chapters"]["blocked"] if d["chapters"] else None
    out["re_identified"] = d["re"]["identified"] if d["re"] else None
    out["gtest_macros"] = d["tests"]["gtest_macros"] if d["tests"] else None
    out["selftest_scenarios"] = d["tests"]["selftest_scenarios"] if d["tests"] else None
    return out


def version_key(tag: str):
    return tuple(int(x) for x in re.findall(r"\d+", tag))


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True, text=True).stdout.strip()


def build_history(root: Path, current: dict, meta: dict) -> list[dict]:
    tags = sorted((t for t in git(root, "tag", "-l", "v*").split("\n") if re.fullmatch(r"v\d+(\.\d+)*", t)),
                  key=version_key)
    batch = GitTree.open_batch(root)
    rows = []
    try:
        for tag in tags:
            d = collect(GitTree(root, tag, batch), strict=False)
            rows.append({"rev": tag, "date": git(root, "log", "-1", "--format=%cs", tag),
                         "sha": git(root, "rev-list", "-n", "1", tag), **summarize(d)})
    finally:
        batch.stdin.close()
        batch.wait()
    rows.append({"rev": "main", "date": meta["date"][:10], "sha": meta["sha"], **summarize(current)})
    return rows


COUNT_SERIES = [
    ("gtest_macros", "gtest TEST macros", "#0969da"),
    ("selftest_scenarios", "Self-test scenarios", "#bf8700"),
]


def history_chart(rows: list[dict], series: list[tuple[str, str, str]], percent: bool) -> str:
    w, h, l, rgt, top, bot = 980, 300, 48, 16, 16, 40
    n = len(rows)
    values = [r[key] for key, _, _ in series for r in rows if r[key] is not None]
    if percent:
        ymax, ticks = 100.0, range(0, 101, 25)
    else:
        step_y = 10 ** max(0, len(str(int(max(values or [1])))) - 1)
        ymax = (int(max(values or [1]) / step_y) + 1) * step_y
        ticks = range(0, int(ymax) + 1, step_y)
    px = lambda i: l + (w - l - rgt) * (i / (n - 1) if n > 1 else 0.5)
    py = lambda v: top + (h - top - bot) * (1 - v / ymax)
    unit = "%" if percent else ""
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="100%" role="img" '
             f'aria-label="Progress per release"><style>text{{font:11px sans-serif;fill:currentColor}} '
             f'.g{{stroke:currentColor;stroke-opacity:.15}}</style>']
    for v in ticks:
        parts.append(f'<line class="g" x1="{l}" x2="{w - rgt}" y1="{py(v):.1f}" y2="{py(v):.1f}"/>'
                     f'<text x="{l - 6}" y="{py(v) + 4:.1f}" text-anchor="end">{v}{unit}</text>')
    step = max(1, n // 12)
    for i, r in enumerate(rows):
        if i % step == 0 or i == n - 1:
            parts.append(f'<text x="{px(i):.1f}" y="{h - bot + 16}" text-anchor="middle">{esc(r["rev"])}</text>')
    for key, label, color in series:
        seg = []
        for i, r in enumerate(rows):
            if r[key] is None:
                if len(seg) > 1:
                    parts.append(f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{" ".join(seg)}"/>')
                seg = []
                continue
            seg.append(f"{px(i):.1f},{py(r[key]):.1f}")
            parts.append(f'<circle cx="{px(i):.1f}" cy="{py(r[key]):.1f}" r="2.5" fill="{color}">'
                         f'<title>{esc(r["rev"])}: {esc(label)} {r[key]}{unit}</title></circle>')
        if len(seg) > 1:
            parts.append(f'<polyline fill="none" stroke="{color}" stroke-width="2" points="{" ".join(seg)}"/>')
    lx = l
    for _, label, color in series:
        parts.append(f'<rect x="{lx}" y="{h - 14}" width="10" height="10" fill="{color}"/>'
                     f'<text x="{lx + 14}" y="{h - 5}">{esc(label)}</text>')
        lx += 14 + text_width(label) * 0.85 + 20
    parts.append("</svg>")
    return "".join(parts)


def history_html(rows: list[dict], meta: dict) -> str:
    def cell(v, fmt=str):
        return "—" if v is None else fmt(v)

    body = [header_html(meta, "OpenUnderworld progress history"),
            '<p><a href="index.html">Current progress</a> · <a href="history.json">history.json</a></p>'
            '<p>One row per release tag <code>v*</code> (plus current <code>main</code>), computed from the files as they '
            'were at that tag. "—" = the source document did not exist yet or had another format then. Sources: '
            f'{a("BRIEF_COMPLIANCE.md", BRIEF)}, {a("BUGS.md", BUGS)}, {a("REFERENCE_COMPARISON.md", COMPARISON)}, '
            f'{a("docs/matrix/", MATRIX_DIR)}, {a("functions.tsv", FUNCTIONS)}, {a("tests/", TESTS_DIR)}.</p>',
            "<h2>Shares</h2>",
            history_chart(rows, [(k, lbl, col) for k, lbl, col, _ in HISTORY_SERIES], percent=True),
            "<h2>Test counts</h2>",
            history_chart(rows, COUNT_SERIES, percent=False),
            "<h2>Per release</h2>",
            '<table><tr><th>Release</th><th>Date</th><th>Brief met</th><th>Bugs fixed</th><th>Measured vs tru.exe</th>'
            '<th>Matching</th><th>Chapter reqs implemented</th><th>Blocked chapters</th><th>RE identified</th>'
            '<th>gtest macros</th><th>Self-tests</th></tr>']
    for r in reversed(rows):
        rev = (f'<a href="{REPO_URL}/releases/tag/{esc(r["rev"])}">{esc(r["rev"])}</a>' if r["rev"] != "main"
               else f'<a href="{REPO_URL}/commit/{esc(r["sha"])}">main</a>')
        bugs = "—" if r["bugs_fixed"] is None else f'{r["bugs_fixed"]}/{r["bugs_total"]}'
        meas = "—" if r["cmp_measured"] is None else f'{r["cmp_measured"]}/{r["cmp_total"]}'
        body.append(f'<tr><td>{rev}</td><td>{esc(r["date"])}</td><td>{cell(r["brief"], pct)}</td><td>{bugs}</td>'
                    f'<td>{meas}</td><td>{cell(r["cmp_matching"])}</td><td>{cell(r["chapters"], pct)}</td>'
                    f'<td>{cell(r["chapters_blocked"])}</td><td>{cell(r["re"], pct)}</td>'
                    f'<td>{cell(r["gtest_macros"])}</td><td>{cell(r["selftest_scenarios"])}</td></tr>')
    body.append("</table>")
    return page("OpenUnderworld progress history", "".join(body))


# --------------------------------------------------------------------------- main


def repo_meta(root: Path) -> dict:
    try:
        return {"sha": git(root, "rev-parse", "HEAD"), "date": git(root, "log", "-1", "--format=%cI")}
    except (subprocess.CalledProcessError, FileNotFoundError):
        return {"sha": "unknown", "date": "unknown"}


def render(d: dict, meta: dict, out: Path, history: list[dict] | None = None) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "progress.json").write_text(json.dumps({"meta": meta, **d}, indent=1) + "\n", encoding="utf-8")
    for name, svg in badges(d).items():
        (out / name).write_text(svg, encoding="utf-8")
    (out / "progress.svg").write_text(progress_svg(d, meta), encoding="utf-8")
    (out / "index.html").write_text(index_html(d, meta, history is not None), encoding="utf-8")
    if history is not None:
        (out / "history.json").write_text(json.dumps(history, indent=1) + "\n", encoding="utf-8")
        (out / "history.html").write_text(history_html(history, meta), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--history", action="store_true", help="also compute history.html / history.json from v* tags")
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = ap.parse_args(argv)
    try:
        d = collect(WorkTree(args.repo), strict=True)
    except ParseError as e:
        print(f"progress.py: {e}", file=sys.stderr)
        return 1
    meta = repo_meta(args.repo)
    history = build_history(args.repo, d, meta) if args.history else None
    render(d, meta, args.outdir, history)
    b, bugs, c, ch, r = d["brief"], d["bugs"], d["comparison"], d["chapters"], d["re"]
    print(f"brief {b['met_pct']:.1f}% met; bugs {bugs['counts']['fixed']}/{bugs['total']} fixed; "
          f"tru.exe {c['matching']}/{c['measured']} measured match ({c['counts']['unmeasured']} unmeasured); "
          f"chapters {ch['count'] - ch['blocked']}/{ch['count']} unblocked, {ch['implemented_pct']:.1f}% reqs; "
          f"RE {r['identified_pct']:.1f}% identified; gtest {d['tests']['gtest_macros']}, "
          f"self-tests {d['tests']['selftest_scenarios']}" + (f"; history {len(history)} rows" if history else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
