#!/usr/bin/env python3
"""drop_summary.py TRACE...: water entry / treading summary of `ref drop` and `ref rec` traces (lines with 'pos' and
'sf'). Per trace: the entry (first sample under the volume top wt: time, z, vertical velocity a 30 Hz frame), the
deepest root under the wave surface sf, the chart request sequence, and for the samples of the tread request (192)
the offset sf - z (mean / min / max) and z - wt (mean / min / max)."""
import sys

def rows(path):
    out = []
    for ln in open(path, errors='replace'):
        p = ln.split()
        if len(p) < 6 or p[1] != 'pos' or 'sf' not in p:
            continue
        g = lambda k, n=1: p[p.index(k) + n]
        out.append(dict(t=float(p[0]), z=float(p[4]), vz=float(g('vel', 3)), rq=int(g('rq')) if 'rq' in p else -1,
                        wt=float(g('wt')), sf=float(g('sf'))))
    return out

def stats(v):
    return '%.2f / %.2f / %.2f (n %d)' % (sum(v) / len(v), min(v), max(v), len(v)) if v else 'none'

for path in sys.argv[1:]:
    r = rows(path)
    print('==', path, len(r), 'samples')
    wet = [s for s in r if s['wt'] > -1e6]
    ent = next((s for s in wet if s['z'] < s['wt']), None)
    if ent:
        print('entry  t %.1f ms z %.2f vz %.2f (top %.2f)' % (ent['t'], ent['z'], ent['vz'], ent['wt']))
    if wet:
        d = min(wet, key=lambda s: s['z'] - s['sf'])
        print('deepest under the waves %.2f at t %.1f ms' % (d['sf'] - d['z'], d['t']))
    seq = []
    for s in r:
        if not seq or seq[-1][0] != s['rq']:
            seq.append((s['rq'], s['t']))
    print('requests', ' '.join('%d@%.0f' % q for q in seq))
    tr = [s for s in wet if s['rq'] == 192]
    print('tread: sf - z', stats([s['sf'] - s['z'] for s in tr]))
    print('tread: z - wt', stats([s['z'] - s['wt'] for s in tr]))
    print('tread: sf - wt', stats([s['sf'] - s['wt'] for s in tr]))
