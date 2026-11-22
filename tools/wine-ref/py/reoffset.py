#!/usr/bin/env python3
"""Re-express traces in our frame with a corrected unit offset (tru = ours + d).

reoffset.py dx,dy,dz CASE.case.json...
For each case: px / py / pz of its TSV become var_tpx/tpy/tpz - d (the raw tru position the tracer logs), spawn.ours and
start.ours are recomputed, offset_tru_minus_ours is replaced (the old value kept as offset_previous) and the notes say so.
"""
import json, sys

d = [float(v) for v in sys.argv[1].split(',')]
for cj in sys.argv[2:]:
    c = json.load(open(cj))
    old = c['offset_tru_minus_ours']
    if old == d:
        continue
    tsv = cj[:-len('.case.json')] + '.tsv'
    lines = open(tsv).read().split('\n')
    out, first = [lines[0]], None
    for line in lines[1:]:
        f = line.split('\t')
        kv = {x.split('=', 1)[0]: x.split('=', 1)[1] for x in f[13:] if '=' in x}
        if len(f) >= 13 and 'var_tpx' in kv:
            for k, col in enumerate(('var_tpx', 'var_tpy', 'var_tpz')):
                f[5 + k] = '%.2f' % (float(kv[col]) - d[k])
            if first is None:
                first = f
        out.append('\t'.join(f))
    open(tsv, 'w').write('\n'.join(out))
    s = c['spawn']
    s['ours'] = [round(s['tru'][k] - d[k], 2) for k in range(3)] + s['ours'][3:]
    if first is not None and 'start' in c:
        c['start']['ours'] = [float(first[5]), float(first[6]), float(first[7]), float(first[11])]
    c['offset_previous'] = old
    c['offset_tru_minus_ours'] = d
    c['notes'] = ('ours coordinates recomputed with the measured offset (%s; recorded with %s, which was wrong). ' %
                  (', '.join('%g' % v for v in d), ', '.join('%g' % v for v in old))) + c['notes']
    json.dump(c, open(cj, 'w'), indent=1)
    print('reoffset', cj)
