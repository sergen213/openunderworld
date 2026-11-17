#!/usr/bin/env python3
"""tl.py FILE LABEL [OX OY OZ]: timeline of one trace segment: a line at every change of the animation id or the
chart state, and at every start / end of a run of non-zero velocity; positions minus (OX, OY, OZ) = our unit frame."""
import sys
path, label = sys.argv[1:3]; off = list(map(float, sys.argv[3:6])) if len(sys.argv) > 5 else [0, 0, 0]
on = False; last = None; lastmov = None; prev = None
for ln in open(path, errors='replace'):
    ln = ln.strip('\r\n')
    if ln.startswith('=='):
        on = ln[2:].strip().startswith(label + ' keys'); 
        if on: print(ln[:100])
        continue
    if not on: continue
    p = ln.split()
    if len(p) > 3 and p[1] == 'key':
        if p[2] != '20' or True: print(f"      {float(p[0]):7.0f} ms  key {p[2]} {p[3]}")
        continue
    if len(p) > 10 and p[1] == 'pos':
        i = p.index('vel'); v = tuple(map(float, p[i+1:i+4])); an = p[p.index('an')+1]; st = p[p.index('st')+1]
        mov = any(abs(c) > 1e-3 for c in v)
        key = (an, st)
        cur = f"{float(p[0]):7.0f} ms  pos {float(p[2])-off[0]:9.1f} {float(p[3])-off[1]:9.1f} {float(p[4])-off[2]:8.1f}  yaw {float(p[8]):6.3f}  vel {v[0]:7.2f} {v[1]:7.2f} {v[2]:7.2f}  an {an} st {st}"
        if key != last or mov != lastmov:
            if prev and prev != cur and (mov != lastmov): print('   .. ' + prev)
            print(('  *   ' if key != last else '      ') + cur)
        last, lastmov, prev = key, mov, cur
if prev: print('  end ' + prev)
