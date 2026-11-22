#!/usr/bin/env python3
# tools/chart-replay.sh traces vs Wine: state sequence (S@tick on the case's tick axis), first divergence, position /
# yaw error. usage: tools/chart-compare.py RUNDIR case... (case = family/name); prints a markdown table row per case.
# SEQ=1 also prints both sequences. The case JSON lookup matches chart-replay.sh (wine_startpos, wine_settle, wine).
import csv, json, math, os, sys
W = '/mnt/1st4TB/Projects/openunderworld-work'
def rows(p):
    out = []
    for r in csv.reader(open(p), delimiter='\t'):
        if not r or r[0] == 'tick': continue
        try: out.append((int(r[0]), int(r[1]), float(r[5]), float(r[6]), float(r[7]), float(r[11])))
        except ValueError: pass
    return out
def seq(rs):
    s, last = [], None
    for t, st, *_ in rs:
        if st != last: s.append((t, st)); last = st
    return s
# Hold-state id ranges (attached families move by alignment, not by the recorded velocity): ledge hangs, poles, perch /
# pillar, beams, ladders.
HOLDS = [(420, 470), (530, 545), (610, 700), (820, 880), (880, 960), (1050, 1100), (1180, 1240)]
def drag_tick(p):
    """A mechanical drag in the Wine take (the harness carrying her): >= 3 consecutive ticks whose horizontal step is
    over 40 units and over 3 x the recorded horizontal speed / 60 + 20, outside hold states. The first such tick, or None."""
    prev, run, first = None, 0, None
    for r in csv.reader(open(p), delimiter='\t'):
        if not r or r[0] == 'tick': continue
        try: t, st, x, y, vx, vy = int(r[0]), int(r[1]), float(r[5]), float(r[6]), float(r[8]), float(r[9])
        except (ValueError, IndexError): continue
        if prev is not None:
            step = math.hypot(x - prev[0], y - prev[1])
            hold = any(a <= st < b for a, b in HOLDS)
            if not hold and step > 40 and step > 3 * math.hypot(vx, vy) / 60 + 20:
                run += 1
                if run == 1: first = t
                if run >= 3: return first
            else:
                run = 0
        prev = (x, y)
    return None
def case(c):
    for d in ('wine_startpos', 'wine_settle', 'wine'):
        p = f'{W}/chart/{d}/{c}.case.json'
        if os.path.exists(p): break
    # the Wine take the case JSON came from (a settle take has its own trace beside it), else the original take
    t = os.path.join(os.path.dirname(p), os.path.basename(c) + '.tsv')
    j = json.load(open(p))
    # the validity marks have one source of truth, the original W/chart/wine/<case>.case.json (the wine_startpos /
    # wine_settle copies predate them): whichever copy supplies the script and positions, the marks come from there
    orig = f'{W}/chart/wine/{c}.case.json'
    if p != orig and os.path.exists(orig):
        o = json.load(open(orig))
        for k in ('invalid_from', 'invalid_until', 'invalid_reason', 'drag_checked'):
            j.pop(k, None)
            if k in o: j[k] = o[k]
    return j, t if os.path.exists(t) else f'{W}/chart/wine/{c}.tsv'
def compare(rundir, c):
    j, wt = case(c)
    sp = j['spawn']; shift = sp['hold_ticks'] + sp.get('settle_ticks', 0)
    ot = f'{rundir}/{os.path.basename(c)}.tsv'
    if not os.path.exists(ot): return f'| {c} | no trace | | | |'
    w = rows(wt); o = [(t - shift, *x) for t, *x in rows(ot) if t - shift >= 0]
    # a case JSON's "invalid_from" (a harness artefact in the Wine take, e.g. the post-SPAWN carry, "invalid_reason"):
    # the ticks from it on are not scored; with "invalid_until" only [invalid_from, invalid_until) is skipped (the take is
    # valid again after it, e.g. ch_hang_land's catch at S533)
    inv = j.get('invalid_from')
    if inv is not None:
        until = j.get('invalid_until')
        ok = (lambda t: t < inv or t >= until) if until is not None else (lambda t: t < inv)
        w = [r for r in w if ok(r[0])]; o = [r for r in o if ok(r[0])]
    ws, os_ = seq(w), seq(o)
    # state match: Wine's sequence states in order with ticks; first divergence
    k = 0; div = ''
    for i, (t, st) in enumerate(ws):
        idle = {35, 39, 40}  # the idle variants a placement settles into (S35 / S39 / S40 differ only by the settle)
        if i < len(os_) and (os_[i][1] == st or (i == 0 and st in idle and os_[i][1] in idle)):
            k += 1; continue
        ours = f'S{os_[i][1]}@{os_[i][0]}' if i < len(os_) else 'end'
        div = f'wine S{st}@{t} ours {ours}'; break
    timing = [abs(a[0] - b[0]) for a, b in zip(ws[:k], os_[:k])]
    om = {t: x for t, *x in o}
    # raw error, and the error relative to tick 0 (the placements differ: Wine's settle moved her off the spawn)
    errs, rel, yerr = [], [], []
    off = None
    for t, st, x, y, z, yaw in w:
        if t in om:
            ox, oy, oz, oyaw = om[t][1:5]
            if off is None: off = (x - ox, y - oy, z - oz)
            errs.append(math.dist((x, y, z), (ox, oy, oz)))
            rel.append(math.dist((x, y, z), (ox + off[0], oy + off[1], oz + off[2])))
            yerr.append(abs((yaw - oyaw + 180) % 360 - 180))
    pe = f'rel mean {sum(rel)/len(rel):.0f} max {max(rel):.0f} end {rel[-1]:.0f} (raw max {max(errs):.0f})' if errs else ''
    ye = f'{max(yerr):.0f}' if yerr else ''
    full = k == len(ws) and len(os_) == len(ws)
    # an unmarked mechanical drag before any invalid_from: not a full match until the case JSON marks it (invalid_from)
    # or clears it ("drag_checked": true)
    dt = None if j.get('drag_checked') else drag_tick(wt)
    drag = dt is not None and (inv is None or dt + 2 < inv)  # the whole 3-tick run before the mark
    if inv is not None:  # a harness-truncated take: never a full match, reported on its own
        span = f'ticks {inv}..{j["invalid_until"]} skipped' if j.get('invalid_until') is not None else f'to tick {inv}'
        res = f'match {span} (truncated)' if full else f'{k}/{len(ws)} {span} (truncated)'
    else:
        res = ('match (drag)' if drag else 'MATCH') if full else f'{k}/{len(ws)}'
    if drag: res += f' WARNING unmarked drag at tick {dt}'
    return f'| {c} | {res} (dt max {max(timing) if timing else 0}) | {div} | {pe} | {ye} |', ws, os_, full and not drag, inv is not None
if __name__ == '__main__':
    d = sys.argv[1]
    print('| case | states (tick slip) | first divergence | position error (units) | yaw err max (deg) |')
    print('|---|---|---|---|---|')
    det = []
    total = fullm = trunc = 0
    for c in sys.argv[2:]:
        r = compare(d, c)
        total += 1
        if isinstance(r, str): print(r); continue
        print(r[0]); det.append((c, r[1], r[2]))
        fullm += r[3] and not r[4]
        trunc += r[3] and r[4]
    # totals: only full-span matches count as MATCH; harness-truncated takes (invalid_from) matching their valid span apart
    print(f'\nfull-span MATCH {fullm} / {total}')
    print(f'match to invalid_from (truncated, not counted) {trunc}')
    if os.environ.get('SEQ'):
        for c, a, b in det:
            print(f'\n{c}\n  wine {" ".join(f"{s}@{t}" for t, s in a)}\n  ours {" ".join(f"{s}@{t}" for t, s in b[:40])}')
