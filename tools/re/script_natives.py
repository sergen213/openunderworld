#!/usr/bin/env python3
"""Recover tru.exe's SCMD native binding table statically and join it with the game's native script types.

tru.exe registers every native script type in one function (REGISTER_FN 0x7A8650): `new NativeScriptType(package,
name, size, align, flag)` (0x574B60, or 0x574B20 for the two-argument form), then the class vftable is stored and
the object is added to the type manager (0x57ED00 on *0xCC4F5C). When a DRM script type section with that native name
is linked, the class's vftable slot 3 ("bind") runs: it calls 0x574BC0 (links the ScriptType, this+0x1C) and then
0x5768F0(index, handler) once per native function, which writes `handler` into
ScriptType->streamData(+8)->functions(+0x38)[index] + 0x14. The SCMD interpreter 0x57CE50 calls that word for
CALLU (0x65) and for the native branch of CALL1..5. So the table is runtime-written but statically determined by the
bind functions; this script reads them from the PE with objdump.

Inputs:
  tru.exe                     (--exe; default $OU_GAME_DIR/tru.exe)
  script_types.tsv            (--types) output of tools/re/script_types_dump.cpp: type id, native name, per function
                              index the prototype's vt slot / argument count / return type, from the 82 unit DRMs
  src/script/Script.cpp       (heuristic "implemented in OpenUnderworld" column)
  docs/re/db/functions.tsv    (Ghidra / applied names of handlers and their callees)
  --runtime FILE              optional capture from tools/re/scriptnat_reader.c ("F" lines: name, index, handler)
Output: docs/re/db/script_natives.tsv (deterministic) and a summary on stdout.
"""
from __future__ import annotations

import argparse
import collections
import csv
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGISTER_FN = 0x7A8650
NEW_TYPE = (0x574B60, 0x574B20)
TYPE_ADD = 0x57ED00
LINK = 0x574BC0
SET_NATIVE = 0x5768F0


class Pe:
    def __init__(self, path: Path):
        import pefile
        self.pe = pefile.PE(str(path), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase

    def u32(self, va: int) -> int:
        return struct.unpack('<I', self.pe.get_data(va - self.base, 4))[0]

    def cstr(self, va: int, n: int = 128) -> str:
        b = self.pe.get_data(va - self.base, n)
        return b.split(b'\0', 1)[0].decode('latin-1')

    def rtti_name(self, vft: int) -> str:
        col = self.u32(vft - 4)
        td = self.u32(col + 0xC)
        raw = self.cstr(td + 8)
        return raw[4:-2] if raw.startswith('.?AV') and raw.endswith('@@') else raw


INS = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')


def disassemble(exe: Path) -> tuple[list[int], dict[int, str]]:
    out = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', str(exe)],
                         check=True, capture_output=True, text=True).stdout
    addrs, text = [], {}
    for line in out.splitlines():
        m = INS.match(line)
        if m:
            a = int(m.group(1), 16)
            addrs.append(a)
            text[a] = ' '.join(m.group(2).split())
    return addrs, text


class Code:
    def __init__(self, addrs, text):
        self.addrs, self.text = addrs, text
        self.index = {a: i for i, a in enumerate(addrs)}

    def walk(self, start: int, limit: int = 4000):
        i = self.index.get(start)
        if i is None:
            return
        for k in range(i, min(i + limit, len(self.addrs))):
            a = self.addrs[k]
            yield a, self.text[a]


def imm(s: str) -> int | None:
    m = re.fullmatch(r'0x[0-9a-f]+|\d+', s)
    return int(s, 0) if m else None


def registrations(pe: Pe, code: Code):
    """(package, name, size, align, flag, vftable) in registration order."""
    pushes, out, pending = [], [], None
    for a, t in code.walk(REGISTER_FN, 20000):
        if t.startswith('push '):
            pushes.append(t[5:])
        elif t.startswith('call '):
            tgt = imm(t[5:])
            if tgt in NEW_TYPE:
                args = [imm(p) for p in reversed(pushes[-5:])] if tgt == NEW_TYPE[0] else \
                       [imm(p) for p in reversed(pushes[-2:])] + [None, None, None]
                pending = (pe.cstr(args[0]), pe.cstr(args[1]), args[2], args[3], args[4])
            pushes = []
        elif pending and t.startswith('mov DWORD PTR [') and '],0x' in t:
            vft = int(t.split('],')[1], 16)
            out.append(pending + (vft,))
            pending = None
        elif t.startswith('ret'):
            break
    return out


def bind_entries(code: Code, bind: int):
    """[(index, handler)], other call targets, and parse notes for one bind function."""
    pushes, regs, entries, others, notes = [], {}, [], [], []
    for a, t in code.walk(bind, 3000):
        if t.startswith('mov ') and ',' in t:
            dst, src = t[4:].split(',', 1)
            if imm(src) is not None:
                regs[dst] = imm(src)
        if t.startswith('push '):
            v = t[5:]
            pushes.append(imm(v) if imm(v) is not None else regs.get(v, v))
        elif t.startswith('call '):
            tgt = imm(t[5:])
            if tgt == SET_NATIVE:
                if len(pushes) >= 2 and isinstance(pushes[-1], int) and isinstance(pushes[-2], int):
                    entries.append((pushes[-1], pushes[-2]))
                else:
                    notes.append(f'unparsed SET_NATIVE at {a:#x}')
            elif tgt != LINK:
                others.append(tgt if tgt is not None else t[5:])
            pushes = []
        elif t.startswith('ret') or t.startswith('jmp '):
            if t.startswith('jmp '):
                notes.append(f'ends in {t}')
            break
    return entries, others, notes


def handler_calls(code: Code, h: int, limit: int = 48) -> list[str]:
    calls = []
    for a, t in code.walk(h, limit):
        if t.startswith('call ') or t.startswith('jmp '):
            tgt = t.split(' ', 1)[1]
            if t.startswith('jmp ') and imm(tgt) is not None and h <= imm(tgt) < h + 0x200:
                continue  # local branch
            calls.append(f'{imm(tgt):#x}' if imm(tgt) is not None else tgt)
            if t.startswith('jmp ') or len(calls) >= 3:
                break
        if t.startswith('ret'):
            break
    return calls


def load_types(path: Path):
    """name -> (type id, {index: (vt, args, ret, native)}, unit count); (type id, index) -> calling script types."""
    types, calls = {}, {}
    with open(path, newline='') as f:
        for row in csv.reader(f, delimiter='\t'):
            if row and row[0] == 'R':
                calls[(int(row[1]), int(row[2]))] = int(row[3])
            if not row or row[0] != 'N':
                continue
            tid, name, idx, vt, args, ret, nat, _words, units = row[1:10]
            t = types.setdefault(name, [int(tid), {}, int(units)])
            t[1][int(idx)] = (int(vt), int(args), int(ret), int(nat))
    return types, calls


def load_names(path: Path):
    names = {}
    if path.exists():
        with open(path, newline='') as f:
            r = csv.DictReader(f, delimiter='\t')
            for row in r:
                n = row.get('applied_name') or row.get('repo_name') or ''
                g = row.get('ghidra_name') or ''
                names[int(row['entry'], 16)] = n if n else (g if g and not g.startswith('FUN_') else '')
    return names


def implemented(script_cpp: Path):
    """Heuristic over the native lambda in Script.cpp: native name -> {index: set of accepted argument counts (None =
    any)} plus names accepted wholesale ('stub': zero result). A binding whose condition demands an argument count
    other than the prototype's never runs (status 'args-mismatch')."""
    src = script_cpp.read_text()
    have = collections.defaultdict(lambda: collections.defaultdict(set))
    stub = set()
    argc = r'(?<![!.\w])(?:nc\.)?args\.(?:size\(\) == (\d+)|(empty)\(\))'
    for m in re.finditer(r'for \(const char\* passive : \{([^}]*)\}\)', src):
        stub.update(re.findall(r'"([a-z0-9_]+)"', m.group(1)))

    def balanced(start: int, open_c: str, close_c: str) -> int:
        depth = 0
        for k in range(start, len(src)):
            if src[k] == open_c:
                depth += 1
            elif src[k] == close_c:
                depth -= 1
                if depth == 0:
                    return k
        return len(src) - 1

    for m in re.finditer(r'strcmp\((?:nc\.)?name, "([a-z0-9_]+)"\)', src):
        name = m.group(1)
        ifp = src.rfind('if (', 0, m.start())
        if ifp < 0 or src.count('\n', ifp, m.start()) > 3:
            continue
        cend = balanced(ifp + 3, '(', ')')
        cond = src[ifp:cend + 1]
        if cond.count('strcmp(') > 1:  # several names: keep this name's clause (up to the next name test)
            own = m.start() - ifp
            nxt = cond.find('strcmp(', own + 1)
            clause = cond[own:nxt if nxt > 0 else len(cond)]
            if 'nc.function' not in clause:  # `(a || b || c) && nc.function == n`: the shared tail applies
                last = cond.rfind('strcmp(')
                clause += cond[cond.find(')', last) + 1:]
            cond = clause
        idx = set(int(x) for x in re.findall(r'nc\.function == (\d+)', cond))
        for lo, hi in re.findall(r'nc\.function >= (\d+) && nc\.function <= (\d+)', cond):
            idx.update(range(int(lo), int(hi) + 1))
        for hi in re.findall(r'nc\.function <= (\d+)(?!\s*\?)', cond):
            if not re.search(r'nc\.function >= \d+ && nc\.function <= ' + hi, cond):
                idx.update(range(0, int(hi) + 1))
        for hi in re.findall(r'nc\.function < (\d+)', cond):
            idx.update(range(0, int(hi)))
        if not idx and 'nc.function' not in cond:
            body_start = src.find('{', cend)
            if body_start >= 0 and src[cend + 1:body_start].strip() == '':
                body = src[body_start:balanced(body_start, '{', '}') + 1]
                # the index under its own name (nc.function, a local alias `fn = nc.function`) and in helpers the
                # block hands nc.function to (`splineNative(*object, nc.function, ...)` -> its `switch (function)`)
                scans = [(body, ['nc\\.function'] + re.findall(r'(\w+) = nc\.function\b', body))]
                for helper in re.findall(r'(\w+)\([^;{}]*\bnc\.function\b', body):
                    d = re.search(r'Runtime::' + helper + r'\(([^)]*)\)\s*(?:const\s*)?\{', src)
                    if d:
                        params = [p.strip().split()[-1].lstrip('&*') for p in d.group(1).split(',') if p.strip()]
                        hbody = src[d.end() - 1:balanced(d.end() - 1, '{', '}') + 1]
                        scans.append((hbody, params))
                for text, vars_ in scans:
                    idx.update(int(x) for x in re.findall(r'case (\d+):', text))
                    for v in vars_:
                        idx.update(int(x) for x in re.findall(r'\b' + v + r' == (\d+)', text))
                        for lo, hi in re.findall(r'\b' + v + r' >= (\d+) && ' + v + r' <= (\d+)', text):
                            idx.update(range(int(lo), int(hi) + 1))
        pairs = re.findall(r'nc\.function == (\d+) && ' + argc, cond)
        glob = re.findall(argc, cond)
        for i in idx:
            own = [int(n) if n else 0 for f, n, e in pairs if int(f) == i]
            if own:
                have[name][i].update(own)
            elif len(glob) == 1 and not pairs:
                have[name][i].add(int(glob[0][0]) if glob[0][0] else 0)
            else:
                have[name][i].add(None)
    # Natives Script.cpp forwards to the World (src/game: setMultibodyNative / setEnemyNative lambdas, waterNative,
    # Enemies::generatorNative): each `if (...)` there names its type with `type == "x"` or is the forwarder's own type.
    forwarders = {'setMultibodyNative': 'multibody', 'setEnemyNative': 'enemybase'}
    for path in sorted((REPO / 'src/game').rglob('*.cpp')):
        gsrc = path.read_text()
        bodies = []
        for fwd, default in forwarders.items():
            for m in re.finditer(r'\b' + fwd + r'\(', gsrc):
                b = gsrc.find('{', m.end())
                bodies.append((gsrc[b:_balanced(gsrc, b) + 1], default))
        for m in re.finditer(r'\b(?:World::waterNative|Enemies::generatorNative)\([^)]*\)\s*\{', gsrc):
            b = m.end() - 1
            bodies.append((gsrc[b:_balanced(gsrc, b) + 1], 'generatorbase' if 'generator' in m.group(0) else None))
        for body, default in bodies:
            for c in re.finditer(r'if \(', body):
                cond = body[c.start():_balanced(body, c.end() - 1, '(', ')') + 1]
                names = re.findall(r'type == "(\w+)"', cond) or ([default] if default else [])
                idx = {int(x) for x in re.findall(r'\bfunction == (\d+)', cond)}
                for lo, hi in re.findall(r'\bfunction >= (\d+) && function <= (\d+)', cond):
                    idx.update(range(int(lo), int(hi) + 1))
                for n in names:
                    for i in idx:
                        have[n][i].add(None)
            for n in ([default] if default else []):
                for i in re.findall(r'case (\d+):', body):
                    have[n][int(i)].add(None)
    return have, stub


def _balanced(text: str, start: int, open_c: str = '{', close_c: str = '}') -> int:
    depth = 0
    for k in range(start, len(text)):
        if text[k] == open_c:
            depth += 1
        elif text[k] == close_c:
            depth -= 1
            if depth == 0:
                return k
    return len(text) - 1


def main() -> int:
    ap = argparse.ArgumentParser()
    gd = os.environ.get('OU_GAME_DIR', '/mnt/1st4TB/Tomb Raider Underworld')
    ap.add_argument('--exe', default=os.path.join(gd, 'tru.exe'))
    ap.add_argument('--types', default=str(REPO.parent / 'openunderworld-work/re-baseline/ScriptNatives/script_types.tsv'))
    ap.add_argument('--runtime', default=None)
    ap.add_argument('--out', default=str(REPO / 'docs/re/db/script_natives.tsv'))
    a = ap.parse_args()

    pe = Pe(Path(a.exe))
    code = Code(*disassemble(Path(a.exe)))
    regs = registrations(pe, code)
    types, script_calls = load_types(Path(a.types))
    names = load_names(REPO / 'docs/re/db/functions.tsv')
    have, stub = implemented(REPO / 'src/script/Script.cpp')
    runtime = {}
    if a.runtime:
        for line in Path(a.runtime).read_text(errors='replace').splitlines():
            p = line.split('\t')
            if p[0] == 'F' and len(p) >= 4:
                runtime[(p[1], int(p[2]))] = int(p[3], 16)

    rows, stats = [], collections.Counter()
    for package, name, size, align, flag, vft in regs:
        cls = pe.rtti_name(vft)
        bind = pe.u32(vft + 12)
        entries, others, notes = bind_entries(code, bind)
        t = types.get(name)
        tid = str(t[0]) if t else '-'
        drm_fns = t[1] if t else {}
        bound = dict(entries)
        stats['types'] += 1
        stats['types_in_drm'] += bool(t)
        indices = sorted(set(bound) | {i for i, v in drm_fns.items() if v[3]})
        if not indices:
            rows.append([tid, name, cls, package, f'{bind:#x}', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-',
                         'no native functions bound' + ('; ' + '; '.join(notes) if notes else '')])
            continue
        for i in indices:
            h = bound.get(i)
            vt, nargs, ret, nat = drm_fns.get(i, (None, None, None, None))
            calls = handler_calls(code, h) if h else []
            callee_names = [names.get(int(c, 16), '') if c.startswith('0x') else '' for c in calls]
            if h is None:
                status = 'unbound'
            elif name in have and i in have[name]:
                accepted = have[name][i]
                status = 'yes' if None in accepted or nargs is None or nargs in accepted else \
                    'args-mismatch(' + '/'.join(str(x) for x in sorted(accepted)) + ')'
            elif name in stub:
                status = 'stub'
            else:
                status = 'no'
            stats['functions'] += 1
            stats[f'impl_{status}'] += 1
            stats['functions_in_drm'] += i in drm_fns
            if t and i in drm_fns:
                stats[f'drm_impl_{status}'] += 1
            ev = [f'bind {bind:#x}']
            if (name, i) in runtime:
                ok = runtime[(name, i)] == h
                ev.append('runtime ' + ('match' if ok else f'MISMATCH {runtime[(name, i)]:#x}'))
                stats['runtime_checked'] += 1
                stats['runtime_match'] += ok
            if others:
                ev.append('bind also calls ' + ','.join(f'{o:#x}' if isinstance(o, int) else str(o) for o in others))
            ev += notes
            rows.append([tid, name, cls, package, f'{bind:#x}', str(i),
                         f'{h:#x}' if h else '-', names.get(h, '') if h else '',
                         ' '.join(calls) or '-', ' '.join(n for n in callee_names if n) or '-',
                         '-' if vt is None else str(vt), '-' if nargs is None else str(nargs),
                         '-' if ret is None else str(ret),
                         str(script_calls.get((t[0], i), 0)) if t else '0', status, '; '.join(ev)])

    def key(r):
        return (int(r[0]) if r[0] != '-' else 1 << 30, r[1], int(r[5]) if r[5] != '-' else -1)
    rows.sort(key=key)
    header = ['type_id', 'native_name', 'class', 'package', 'bind_fn', 'fn_index', 'handler', 'handler_name',
              'handler_calls', 'callee_names', 'proto_vt', 'args', 'ret_type', 'called_by_types', 'implemented_ou',
              'evidence']
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, 'w', newline='') as f:
        w = csv.writer(f, delimiter='\t', lineterminator='\n')
        w.writerow(header)
        w.writerows(rows)
    for k in sorted(stats):
        print(f'{k}\t{stats[k]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
