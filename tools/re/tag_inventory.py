#!/usr/bin/env python3
"""Inventory the function definitions in src/game and src/script by evidence tag (master brief P5-3).

A code path here is one function definition in a .cpp file (namespace-scope functions, member functions defined out
of line, and functions defined inside classes or structs local to the .cpp). Its evidence is searched in:
  - the comment block directly above the definition (contiguous comment lines, blank lines not allowed),
  - every comment inside its body,
  - the comment block above, or trailing, a declaration of the same name in a .hpp of the same directory tree.
Tags: [O ...] observed (incl. [O-data], [O Wine], [O decomp]), [I ...] / [INFERENCE] inference, [U ...] unknown.
A tru.exe address (0x + 6 hex digits in a comment) without a tag is counted separately.

Output: docs/re/db/untagged_paths.tsv, one row per definition without a tag:
  path, line, end, lines (body lines), function, status (`address-only` = cites a tru.exe address, `untagged` =
  neither), trivial (1 = body of at most 3 lines: accessors, forwarding wrappers)
and a summary on stdout (per file: definitions, tagged, address-only, untagged, untagged non-trivial).

Heuristic parser (braces, comments and string literals; no preprocessor): it does not judge whether a path NEEDS
evidence (helpers, rendering glue and test hooks have no original counterpart); the list is a work queue, not a
defect list.

Usage: tools/re/tag_inventory.py [--out FILE] [--roots src/game src/script]
"""
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TAG = re.compile(r"\[(?:O|I|U)(?:\]|[\s:,;/\-]|NFERENCE)")
ADDR = re.compile(r"0x[0-9A-Fa-f]{6}\b")
CONTROL = {"if", "for", "while", "switch", "catch", "return", "sizeof", "decltype", "alignof", "requires", "else", "do"}
NAME = re.compile(r"(~?[A-Za-z_][A-Za-z0-9_]*(?:::~?[A-Za-z_][A-Za-z0-9_]*)*|operator\s*[^\s(]+)\s*\($")


def strip(text: str) -> tuple[list[str], list[str]]:
    """Per line: code with comments / literals blanked, and the comment text."""
    code, comm = [], []
    in_block = False
    for line in text.split("\n"):
        c, k = [], []
        i, n = 0, len(line)
        quote = None
        while i < n:
            ch = line[i]
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    k.append(line[i:])
                    i = n
                else:
                    k.append(line[i:j])
                    i = j + 2
                    in_block = False
                continue
            if quote:
                if ch == "\\":
                    i += 2
                    continue
                if ch == quote:
                    quote = None
                c.append(" ")
                i += 1
                continue
            if line.startswith("//", i):
                k.append(line[i + 2:])
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if ch in "\"'":
                # raw string literals R"(...)" are rare in these trees; a digit separator ' (1'000) is not a quote
                if ch == "'" and i > 0 and line[i - 1].isalnum() and i + 1 < n and line[i + 1].isalnum():
                    c.append(" ")
                    i += 1
                    continue
                quote = ch
                c.append(" ")
                i += 1
                continue
            c.append(ch)
            i += 1
        code.append("".join(c))
        comm.append(" ".join(k))
    return code, comm


def definitions(code: list[str]):
    """(start line, end line, name) for each function body opened at namespace or class level."""
    out = []
    stack = []  # 'ns' | 'class' | 'func' | 'block'
    header = []  # code since the last ; { }
    header_line = 0
    func_start = {}
    for ln, line in enumerate(code):
        if line.lstrip().startswith("#"):
            continue  # preprocessor
        for ch in line:
            if ch == "{":
                h = " ".join("".join(header).split())
                kind = "block"
                if not stack or stack[-1] in ("ns", "class"):
                    if re.match(r"^(inline\s+)?namespace\b", h) or h.startswith('extern "C"') or h == "":
                        kind = "ns" if h else "block"
                    elif re.search(r"\b(class|struct|union)\b[^()]*$", h) or re.match(r"^enum\b", h):
                        kind = "class"
                    elif ")" in h and "=" not in h.split("(")[0]:
                        sig = h[: h.find("(")].strip() + "("
                        m = NAME.search(sig)
                        if m and m.group(1).split("::")[-1] not in CONTROL:
                            kind = "func"
                            func_start[len(stack)] = (header_line, m.group(1))
                    elif re.search(r"\)\s*(const|noexcept|override|final|->[^{]*)*$", h):
                        kind = "func"
                        func_start[len(stack)] = (header_line, "?")
                stack.append(kind)
                header = []
                header_line = ln + 1
            elif ch == "}":
                if stack:
                    kind = stack.pop()
                    if kind == "func":
                        s, name = func_start.pop(len(stack))
                        out.append((s, ln, name))
                header = []
                header_line = ln + 1
            elif ch == ";":
                if not stack or stack[-1] in ("ns", "class"):
                    header = []
                    header_line = ln + 1
                else:
                    header.append(ch)
            else:
                if not header and not ch.isspace():
                    header_line = ln
                header.append(ch)
        header.append(" ")
    return out


def header_tags(roots: list[Path]) -> dict[str, bool]:
    """Simple name -> a .hpp declaration of it carries a tag (block above or trailing comment)."""
    tagged = collections.defaultdict(bool)
    for root in roots:
        for p in sorted(root.rglob("*.hpp")):
            code, comm = strip(p.read_text(encoding="utf-8", errors="replace"))
            for i, line in enumerate(code):
                for m in re.finditer(r"(~?[A-Za-z_][A-Za-z0-9_]*)\s*\(", line):
                    name = m.group(1)
                    if name in CONTROL:
                        continue
                    text = comm[i]
                    j = i - 1
                    while j >= 0 and comm[j].strip() and not code[j].strip():
                        text += " " + comm[j]
                        j -= 1
                    if TAG.search(text):
                        tagged[name] = True
    return tagged


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=REPO / "docs/re/db/untagged_paths.tsv")
    ap.add_argument("--roots", nargs="+", default=["src/game", "src/script"])
    args = ap.parse_args()
    roots = [REPO / r for r in args.roots]
    htag = header_tags(roots)
    rows = []
    per = collections.OrderedDict()
    for root in roots:
        for p in sorted(root.rglob("*.cpp")):
            rel = p.relative_to(REPO).as_posix()
            code, comm = strip(p.read_text(encoding="utf-8", errors="replace"))
            st = per.setdefault(rel, collections.Counter())
            for s, e, name in definitions(code):
                above = []
                j = s - 1
                while j >= 0 and comm[j].strip() and not code[j].strip():
                    above.append(comm[j])
                    j -= 1
                text = " ".join(above) + " " + " ".join(comm[s:e + 1])
                simple = name.split("::")[-1]
                body = e - s
                st["defs"] += 1
                if TAG.search(text) or htag.get(simple, False):
                    st["tagged"] += 1
                    continue
                status = "address-only" if ADDR.search(text) else "untagged"
                trivial = 1 if body <= 3 else 0
                st[status] += 1
                if status == "untagged" and not trivial:
                    st["untagged_nontrivial"] += 1
                rows.append((rel, s + 1, e + 1, body, name, status, trivial))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="") as f:
        f.write("path\tline\tend\tlines\tfunction\tstatus\ttrivial\n")
        for r in rows:
            f.write("\t".join(map(str, r)) + "\n")
    tot = collections.Counter()
    print("file\tdefs\ttagged\taddress-only\tuntagged\tuntagged_nontrivial")
    for rel, st in per.items():
        tot.update(st)
        print(f"{rel}\t{st['defs']}\t{st['tagged']}\t{st['address-only']}\t{st['untagged']}\t{st['untagged_nontrivial']}")
    print(f"total\t{tot['defs']}\t{tot['tagged']}\t{tot['address-only']}\t{tot['untagged']}\t{tot['untagged_nontrivial']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
