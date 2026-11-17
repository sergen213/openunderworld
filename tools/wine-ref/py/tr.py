#!/usr/bin/env python3
"""tr.py FILE...: summary of pt/wmem traces (tru.exe under Wine). Ground speed = least-squares slope of the path
length over time; flights = runs of samples with non-zero +0x2A0 velocity (units per 30 Hz frame)."""
import sys, math, re
def parse(path):
    segs, cur = [], None
    for ln in open(path, errors='replace'):
        ln = ln.strip('\r\n')
        if ln.startswith('=='):
            cur = {'label': ln[2:].strip(), 's': [], 'keys': []}; segs.append(cur); continue
        if cur is None: cur = {'label': path.split('/')[-1], 's': [], 'keys': []}; segs.append(cur)
        p = ln.split()
        if len(p) > 3 and p[1] == 'key': cur['keys'].append((float(p[0]), p[2], p[3])); continue
        if len(p) > 10 and p[1] == 'pos':
            try:
                t = float(p[0]); x, y, z = map(float, p[2:5]); i = p.index('vel'); v = tuple(map(float, p[i+1:i+4]))
                r = float(p[p.index('rot')+3]) if 'rot' in p else 0.0
                an = int(p[p.index('an')+1]) if 'an' in p else -1
                st = p[p.index('st')+1] if 'st' in p else ''
                cur['s'].append((t, x, y, z, v, r, an, st))
            except ValueError: pass
    return segs
def slope(ts, ds):
    n = len(ts)
    if n < 3: return float('nan')
    mt, md = sum(ts)/n, sum(ds)/n
    den = sum((t-mt)**2 for t in ts)
    return sum((t-mt)*(d-md) for t, d in zip(ts, ds))/den*1000 if den else float('nan')
def summary(seg):
    s = seg['s']
    if len(s) < 3: return
    print(f"## {seg['label']}: {len(s)} samples, {s[0][0]:.0f}..{s[-1][0]:.0f} ms; keys " + ' '.join(f"{k}{'+' if d=='down' else '-'}@{t:.0f}" for t, k, d in seg['keys']))
    # flights
    i, n = 0, len(s)
    while i < n:
        if any(abs(c) > 1e-3 for c in s[i][4]):
            j = i
            while j + 1 < n and any(abs(c) > 1e-3 for c in s[j+1][4]): j += 1
            a, b = s[max(i-1, 0)], s[j]
            zmax = max(q[3] for q in s[i:j+1]); tz = [q[0] for q in s[i:j+1] if q[3] == zmax][0]
            hd = math.hypot(b[1]-a[1], b[2]-a[2]); v0 = s[i][4]
            vh = max(math.hypot(q[4][0], q[4][1]) for q in s[i:j+1])
            # ground speed over the 400 ms before take-off
            g = [q for q in s[:i] if a[0]-400 <= q[0] <= a[0]]
            d, ds = 0, []
            for k, q in enumerate(g):
                if k: d += math.hypot(q[1]-g[k-1][1], q[2]-g[k-1][2])
                ds.append(d)
            gs = slope([q[0] for q in g], ds)
            press = [t for t, k, dd in seg['keys'] if k == '20' and dd == 'down' and t <= s[i][0]]
            print(f"  flight {a[0]:.0f}..{b[0]:.0f} ms ({(b[0]-a[0])/1000:.3f} s){' press+%.0f ms' % (a[0]-press[-1]) if press else ''}: first vel ({v0[0]:.2f}, {v0[1]:.2f}, {v0[2]:.2f})/frame = up {v0[2]*30:.1f} u/s, max horiz {vh:.2f}/frame = {vh*30:.1f} u/s; apex +{zmax-a[3]:.1f} at +{tz-a[0]:.0f} ms; horizontal {hd:.1f}; dz {b[3]-a[3]:.1f}; ground speed before {gs:.1f} u/s; an {s[i][6]}..{s[j][6]}")
            i = j + 1
        else: i += 1
    # steady ground speeds: 500 ms windows with zero velocity field
    best = 0; w = []
    k0 = 0
    for k in range(n):
        while s[k][0] - s[k0][0] > 500: k0 += 1
        if s[k][0] - s[k0][0] > 450 and all(abs(c) < 1e-3 for q in s[k0:k+1] for c in q[4]):
            d, ds = 0, []
            for m in range(k0, k+1):
                if m > k0: d += math.hypot(s[m][1]-s[m-1][1], s[m][2]-s[m-1][2])
                ds.append(d)
            w.append((s[k][0], slope([q[0] for q in s[k0:k+1]], ds)))
    if w:
        sp = sorted(v for _, v in w)
        print(f"  ground speed over 0.5 s windows: max {sp[-1]:.1f}, median {sp[len(sp)//2]:.1f} u/s; by time: " + ' '.join(f"{t/1000:.1f}s:{v:.0f}" for t, v in w[::max(1, len(w)//14)]))
for f in sys.argv[1:]:
    print('#', f)
    for seg in parse(f): summary(seg)
