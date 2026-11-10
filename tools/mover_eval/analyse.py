#!/usr/bin/env python3
# Mover evaluation: reads out/*.trace (tick state x y z) and out/*.log; prints the tables of PHYSICS.md "Character controller".
import re, os, sys
W = (sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)) + "/../../build/mover_eval") + "/"
MOVERS = ['sweep', 'jolt', 'jolt-capsule']
def trace(name, m):
    rows = []
    try:
        for l in open(W + (f'gtest.{m}.{name}' if name in ('stairs', 'hearth') else f'{name}.{m}.trace')):
            t, st, x, y, z = l.split()
            if rows and int(t) <= rows[-1][0]: break  # a load restarted the tick count: the run up to it
            rows.append((int(t), st, float(x), float(y), float(z)))
    except FileNotFoundError:
        pass
    return rows
# tru.exe under Wine, screens/stairs/t4.txt: y interval (tru frame) in which z jumped by a whole riser between two samples
TRU = [(-5442.17, -5463.44), (-5618.38, -5639.63), (-5808.21, -5829.5), (-6000.1, -6021.32), (-6188.06, -6204.91),
       (-6373.41, -6390.26), (-6558.74, -6575.58), (-6753.68, -6768.38), (-6936.83, -6953.68), (-7122.13, -7138.98)]
TRUZ = [-2117.74, -2096.16, -1996.79, -1902.28, -1802.32, -1704.52, -1608.07, -1510.16, -1412.33, -1314.71, -1204.99]
OFF = (9622.0, 333.0, -2977.0)  # tru = ours + OFF
print('## stairs (l3_main_bedroom, run up): y (our frame) where z reaches each tread, lead before tru.exe\'s jump interval')
for m in MOVERS:
    rows = trace('stairs', m)
    if not rows: continue
    leads, ticks_per, speed = [], [], []
    for i, (a, b) in enumerate(TRU):
        za, zb = TRUZ[i] - OFF[2], TRUZ[i + 1] - OFF[2]
        mid, hi = 0.5 * (za + zb), zb - 3
        first = next((r for r in rows if r[4] > za + 3), None)   # leaves the lower tread
        half = next((r for r in rows if r[4] > mid), None)
        done = next((r for r in rows if r[4] > hi), None)
        if not (first and half and done): leads.append(None); continue
        ymid_tru = 0.5 * (a + b) - OFF[1]
        leads.append((first[3] - ymid_tru, done[3] - ymid_tru, done[0] - first[0] + 1))
    ys = [r[3] for r in rows]
    run = [abs(b[3] - a[3]) for a, b in zip(rows[60:], rows[61:]) if b[1].lower() == 'run']
    falls = sum(1 for a, b in zip(rows, rows[1:]) if a[1] != b[1] and b[1].lower() == 'fall')
    print(f'{m:13s} final z {rows[-1][4]:.1f} y {rows[-1][3]:.1f} falls {falls} mean |dy|/tick while running {sum(run)/max(len(run),1):.2f} min {min(run) if run else 0:.2f}')
    for i, l in enumerate(leads):
        print(f'   riser {i}: ' + ('not climbed' if l is None else f'starts rising {l[0]:+.1f} units before tru\'s jump, on the tread {l[1]:+.1f}, over {l[2]} tick(s)'))
print('\n## hearth (121 units): stop distance from its edge x = -4853')
for m in MOVERS:
    rows = trace('hearth', m)
    if rows: print(f'{m:13s} x {rows[-1][2]:.2f} stops {rows[-1][2] + 4853:.2f} short, z {rows[-1][4]:.2f} state {rows[-1][1]}')
for name in ['pool_to_ragnarok', 'save_move_load']:
    print(f'\n## {name}: per-tick position difference to the sweep mover; state changes')
    base = {r[0]: r for r in trace(name, 'sweep')}
    for m in MOVERS:
        rows = trace(name, m)
        if not rows: continue
        d = [((r[2] - base[r[0]][2]) ** 2 + (r[3] - base[r[0]][3]) ** 2 + (r[4] - base[r[0]][4]) ** 2) ** 0.5 for r in rows if r[0] in base]
        dz = [abs(r[4] - base[r[0]][4]) for r in rows if r[0] in base]
        ch = [(a[1], b[1]) for a, b in zip(rows, rows[1:]) if a[1] != b[1]]
        fl = sum(1 for c in ch if 'fall' in (c[0].lower(), c[1].lower()) or 'land' in (c[0].lower(), c[1].lower()))
        print(f'{m:13s} ticks {len(rows)} diff mean {sum(d)/max(len(d),1):.2f} max {max(d) if d else 0:.2f} final {d[-1] if d else 0:.2f} |dz| max {max(dz) if dz else 0:.2f}; '
              f'state changes {len(ch)}, into/out of Fall/Land {fl}: ' + ' '.join(f'{a}>{b}' for a, b in ch[:14]))
        for l in open(W + f'{name}.{m}.log'):
            if 'final tick' in l or 'saved' in l or 'loaded' in l: print('      ' + l.strip()[:200])
print('\n## tick cost (ou-game --frames 600, ms per tick)')
for name in ['cost_pool', 'cost_junglehub']:
    for m in MOVERS:
        try:
            for l in open(W + f'{name}.{m}.log'):
                if 'tick time' in l: print(f'{name:15s} {m:13s} ' + l.strip().replace('ou-game: ', ''))
        except FileNotFoundError: pass
