#!/usr/bin/env python3
"""swim_summary.py TRACE [STEP_MS]: reduce a `ref rec` trace (tru.exe under Wine) to a timeline every STEP_MS
(default 100): time, root, 3D / horizontal / vertical speed from the root's motion over the step (units/s), heading
(rot z) and pitch (rot x) in degrees with their rates (deg/s), chart request, breath (+0x920, frames) and health,
camera mode, distance, pivot z minus root z. Key events are printed where they fall. A last block gives the
breath drain (frames/s over the samples with air falling), the time air reached 0 and the health steps after it."""
import sys, math

path = sys.argv[1]; step = float(sys.argv[2]) if len(sys.argv) > 2 else 100.0
S, K = [], []
for ln in open(path, errors='replace'):
    p = ln.split()
    if len(p) > 3 and p[1] == 'key':
        K.append((float(p[0]), p[2], p[3])); continue
    if len(p) < 10 or p[1] != 'pos':
        continue
    g = lambda k, n=1: float(p[p.index(k) + n])
    S.append(dict(t=float(p[0]), x=g('pos'), y=g('pos', 2), z=g('pos', 3), rx=g('rot'), rz=g('rot', 3),
                  rq=int(g('rq')), air=g('air'), hp=g('hp'), m=int(g('m')), d=g('d'), pz=g('piv', 3), cz=g('cam', 3)))
if not S:
    sys.exit('no rec samples')

def at(t):  # last sample at or before t
    best = S[0]
    for s in S:
        if s['t'] > t:
            break
        best = s
    return best

def unwrap(a, b):
    d = b - a
    while d > math.pi: d -= 2 * math.pi
    while d < -math.pi: d += 2 * math.pi
    return d

print('   t ms        x        y        z   v3d  vxy    vz   head  dhead  pitch dpitch   rq   air     hp  m      d  piv-z')
t, ki, end = 0.0, 0, S[-1]['t']
while t <= end:
    a, b = at(t), at(t + step)
    dt = (b['t'] - a['t']) / 1000.0
    while ki < len(K) and K[ki][0] < t + step:
        print('  -- %.0f key %s %s' % K[ki]); ki += 1
    if dt > 0:
        dx, dy, dz = b['x'] - a['x'], b['y'] - a['y'], b['z'] - a['z']
        v3, vxy = math.sqrt(dx * dx + dy * dy + dz * dz) / dt, math.hypot(dx, dy) / dt
        print('%7.0f %8.1f %8.1f %8.1f %5.0f %4.0f %5.0f %6.1f %6.1f %6.1f %6.1f %4d %5.0f %6.1f %2d %6.1f %6.1f' % (
            a['t'], a['x'], a['y'], a['z'], v3, vxy, dz / dt, math.degrees(a['rz']), math.degrees(unwrap(a['rz'], b['rz'])) / dt,
            math.degrees(a['rx']), math.degrees(unwrap(a['rx'], b['rx'])) / dt, a['rq'], a['air'], a['hp'], a['m'], a['d'],
            a['pz'] - a['z']))
    t += step
fall = [(S[i - 1], S[i]) for i in range(1, len(S)) if S[i]['air'] < S[i - 1]['air'] and S[i]['air'] > 0]
if len(fall) > 10:
    a, b = fall[0][0], fall[-1][1]
    print('breath: %.1f -> %.1f frames in %.2f s = %.2f frames/s (from %.0f ms)' % (
        a['air'], b['air'], (b['t'] - a['t']) / 1000, (a['air'] - b['air']) / ((b['t'] - a['t']) / 1000), a['t']))
    z0 = next((s for s in S if s['t'] > a['t'] and s['air'] <= 0), None)
    if z0:
        print('air <= 0 at %.0f ms (%.2f s after the drain began)' % (z0['t'], (z0['t'] - a['t']) / 1000))
    hp = [(S[i]['t'], S[i - 1]['hp'], S[i]['hp']) for i in range(1, len(S)) if S[i]['hp'] < S[i - 1]['hp']]
    for h in hp[:12]:
        print('health %.1f -> %.1f at %.0f ms' % (h[1], h[2], h[0]))
