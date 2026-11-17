#!/usr/bin/env python3
"""swing.py tru FILE LABEL PX PY PZ | swing.py ours LOG STEP PX PY PZ
Pendulum summary of a trace about the pivot (the ring): turning points (time, offset along x, height under the pivot,
angle of root+HAND from straight down), passes under the pivot (time, speed), mean period."""
import sys, math, re
mode, path, label = sys.argv[1:4]; px, py, pz = map(float, sys.argv[4:7])
HAND = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0
S = []
if mode == 'tru':
    on = False
    for ln in open(path, errors='replace'):
        ln = ln.strip('\r\n')
        if ln.startswith('=='): on = ln[2:].strip().startswith(label + ' keys') or ln[2:].strip().startswith(label + ' lifted'); continue
        p = ln.split()
        if on and len(p) > 10 and p[1] == 'pos':
            i = p.index('vel'); v = [float(c)*30 for c in p[i+1:i+4]]
            S.append((float(p[0])/1000, float(p[2]), float(p[3]), float(p[4]), v, p[p.index('an')+1] if 'an' in p else ''))
        elif on and len(p) > 3 and p[1] == 'key': print(f"  key {p[2]} {p[3]} at {float(p[0])/1000:.2f} s")
else:
    t0 = None
    for ln in open(path, errors='replace'):
        m = re.search(r'self-test (\S+) t (\d+) (\S+) (\S+) pos (\S+) (\S+) (\S+) yaw (\S+) speed (\S+) vz (\S+)', ln)
        if m and m.group(1) == label:
            t = int(m.group(2)); t0 = t if t0 is None else t0
            S.append(((t-t0)/60, float(m.group(5)), float(m.group(6)), float(m.group(7)), None, m.group(3)+':'+m.group(4)))
if len(S) < 5: print('no samples'); sys.exit()
def sp(i):
    if S[i][4] is not None: return math.sqrt(sum(c*c for c in S[i][4]))
    j = max(i-1, 0); k = min(i+1, len(S)-1); dt = S[k][0]-S[j][0]
    return math.dist(S[j][1:4], S[k][1:4])/dt if dt > 0 else 0
last = None
for i, q in enumerate(S):
    if q[5] != last: print(f"  {q[0]:6.2f} s  an/state {q[5]:<44} pos {q[1]-px:8.1f} {q[2]-py:8.1f} {q[3]-pz:8.1f} (from the pivot)  speed {sp(i):7.1f}"); last = q[5]
# turning points and bottom passes along x
off = [q[1]-px for q in S]; tp = []; bp = []
for i in range(2, len(S)-2):
    r = math.dist((S[i][1], S[i][2], S[i][3]+HAND), (px, py, pz))
    if abs(off[i]) > 150 and ((off[i] >= off[i-1] and off[i] > off[i+1] and off[i] >= off[i-2] and off[i] >= off[i+2] and off[i] > 0) or (off[i] <= off[i-1] and off[i] < off[i+1] and off[i] <= off[i-2] and off[i] <= off[i+2] and off[i] < 0)):
        if tp and S[i][0]-tp[-1][0] < 0.5: continue
        ang = math.degrees(math.atan2(math.hypot(off[i], S[i][2]-py), pz-(S[i][3]+HAND)))
        tp.append((S[i][0], off[i], S[i][3]-pz, ang, r))
    if off[i-1]*off[i] < 0 and abs(S[i][3]-pz) > 300: bp.append((S[i][0], sp(i), S[i][3]-pz, r))
for t, o, z, a, r in tp: print(f"  turn  {t:6.2f} s  x {o:8.1f}  z {z:8.1f}  angle {a:5.1f} deg  rope(root+{HAND:.0f}) {r:7.1f}")
for t, v, z, r in bp: print(f"  under {t:6.2f} s  speed {v:7.1f} u/s ({v/30:6.2f}/frame)  z {z:8.1f}  rope {r:7.1f}")
if len(tp) > 2: print(f"  mean half period {sum(tp[i+1][0]-tp[i][0] for i in range(len(tp)-1))/(len(tp)-1):.3f} s over {len(tp)-1} half swings")
