#!/usr/bin/env python3
"""Unit offset (tru = ours + d) from instances present in both frames.

offset.py OURS_INTROS.txt TRU_INST.txt [-v]
  OURS_INTROS.txt: `uid objectId x y z` per intro of the unit in our frame (tools/wine-ref/ours/intros.cpp)
  TRU_INST.txt:    `wmem inst` output after the goto (`addr uid x y z name`)
Every uid in both gives a candidate d = tru - ours; objects that moved since their birth (doors, movers, enemies, Lara)
disagree, so d is the vector most candidates agree with within 0.5 units (ties: the most common, then the median).
Prints `dx,dy,dz count/total`; exits 1 with fewer than 3 agreeing instances.
"""
import sys


def load(path, cols):
    out = {}
    for line in open(path, errors='replace'):
        f = line.split()
        if len(f) < max(cols) + 1:
            continue
        try:
            uid = int(f[cols[0]])
            out[uid] = tuple(float(f[c]) for c in cols[1:])
        except ValueError:
            continue
    return out


ours = load(sys.argv[1], (0, 2, 3, 4))
tru = load(sys.argv[2], (1, 2, 3, 4))
cand = [(u, tuple(tru[u][k] - ours[u][k] for k in range(3))) for u in ours if u in tru and u > 0]
best, bestN = None, 0
for _, d in cand:
    n = sum(1 for _, e in cand if all(abs(e[k] - d[k]) < 0.5 for k in range(3)))
    if n > bestN:
        best, bestN = d, n
if not best or bestN < 3:
    print('no offset: %d agreeing of %d shared instances' % (bestN, len(cand)))
    sys.exit(1)
agree = sorted(e for _, e in cand if all(abs(e[k] - best[k]) < 0.5 for k in range(3)))
med = [sorted(e[k] for e in agree)[len(agree) // 2] for k in range(3)]
print('%.2f,%.2f,%.2f %d/%d' % (med[0], med[1], med[2], bestN, len(cand)))
if '-v' in sys.argv:
    for u, d in cand:
        print(u, ' '.join('%.2f' % v for v in d))
