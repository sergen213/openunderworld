#!/usr/bin/env python3
"""Sibling .case.json of a `chart trace` TSV (contract: W/chart/wine/README.md) and a state summary.

chart_case.py write CASE.json --unit U --offset dx,dy,dz --spawn x,y,z,yaw (tru frame, yaw radians) --hold N
              --ticks N --cam dx,dy,dist,h [--notes TEXT] -- vk:down:up ...
chart_case.py summary TRACE.tsv...   state sequence (S###@tick, request, parent) and flights per trace
chart_case.py index ROOT            markdown table of every ROOT/<family>/<case>.case.json and its state sequence
chart_case.py worldvel TRACE.tsv... convert vx / vy of traces from before the world-velocity change (idempotent)

tru = ours + offset; tru yaw = ours + 90 deg (docs/REFERENCE_COMPARISON.md "Coordinates"). With the TSV next to the case,
`start` records row 0 (ours position, yaw in degrees, chart state): where tick 0 begins after the pin and the SETTLE frames.
"""
import json, math, sys

MASK = {0x26: 1, 0x57: 1, 0x28: 2, 0x53: 2, 0x25: 4, 0x41: 4, 0x27: 8, 0x44: 8, 0x20: 0x10, 0x45: 0x20, 0x11: 0x40,
        0xA2: 0x40, 0xA0: 0x80, 0x10: 0x80, 0xA4: 0x100, 0x12: 0x100, 0x51: 0x200}


def script(keys):
    ev = []
    for k in keys:
        vk, dn, up = k.split(':')
        m = MASK.get(int(vk, 16), 0)
        ev += [(int(dn), m, 1), (int(up), m, 0)]
    ev.sort()
    mask, out = 0, [[0, 0]]
    for t, m, d in ev:
        mask = mask | m if d else mask & ~m
        if out[-1][0] == t:
            out[-1][1] = mask
        else:
            out.append([t, mask])
    return out


def write(argv):
    path, a, keys = argv[0], {}, []
    i = 1
    while i < len(argv):
        if argv[i] == '--':
            keys = argv[i + 1:]
            break
        a[argv[i][2:]] = argv[i + 1]
        i += 2
    off = [float(v) for v in a['offset'].split(',')]
    sp = [float(v) for v in a['spawn'].split(',')]
    yaw = sp[3] if len(sp) > 3 else None
    ours = [sp[k] - off[k] for k in range(3)]
    case = {
        'unit': a['unit'],
        'offset_tru_minus_ours': off,
        'spawn': {'tru': sp[:3] + ([yaw] if yaw is not None else []),
                  'ours': [round(v, 2) for v in ours] + ([round(math.degrees(yaw) - 90.0, 3)] if yaw is not None else []),
                  'tru_yaw_rad': yaw, 'ours_yaw_deg': None if yaw is None else round(math.degrees(yaw) - 90.0, 3),
                  'hold_ticks': int(a.get('hold', 20)), 'settle_ticks': int(a.get('settle', 0))},
        'camera_follow': a.get('cam'),
        'ticks': int(a['ticks']),
        'keys_vk_down_up_ticks': keys,
        'input': script(keys),
        'notes': a.get('notes', ''),
    }
    tsv = path[:-len('.case.json')] + '.tsv'
    try:
        r0 = next(rows(tsv))
        case['start'] = {'ours': [float(r0['px']), float(r0['py']), float(r0['pz']), float(r0['yaw'])],
                         'state': int(r0['state']), 'request': int(r0['request'])}
    except (OSError, StopIteration):
        pass
    with open(path, 'w') as f:
        json.dump(case, f, indent=1)
        f.write('\n')


def rows(path):
    with open(path) as f:
        hdr = f.readline().rstrip('\n').split('\t')
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) < 13 or not c[0].lstrip('-').isdigit():
                continue
            r = dict(zip(hdr, c[:13]))
            for x in c[13:]:
                if '=' in x:
                    k, v = x.split('=', 1)
                    r[k] = v
            yield r


def summary(paths):
    for p in paths:
        seq, last, n = [], None, 0
        for r in rows(p):
            n += 1
            if r['state'] != last:
                seq.append('S%s@%s(rq%s)' % (r['state'], r['tick'], r['request']))
                last = r['state']
        print('%s: %d ticks, %d states: %s' % (p, n, len(seq), ' '.join(seq)))


def index(root):
    import glob, os
    print('| family | case | unit | ticks | spawn ours (x, y, z, yaw deg) | input script [tick, mask] | state sequence (S@tick) | notes |')
    print('|---|---|---|---|---|---|---|---|')
    for cj in sorted(glob.glob(os.path.join(root, '*', '*.case.json'))):
        c = json.load(open(cj))
        tsv = cj[:-len('.case.json')] + '.tsv'
        seq, last = [], None
        if os.path.exists(tsv):
            for r in rows(tsv):
                if r['state'] != last:
                    seq.append('%s@%s' % (r['state'], r['tick']))
                    last = r['state']
        fam = os.path.basename(os.path.dirname(cj))
        name = os.path.basename(tsv)[:-4]
        sp = c['spawn']['ours']
        print('| %s | `%s` | %s | %d | %s | %s | %s | %s |' % (
            fam, name, c['unit'], c['ticks'], ', '.join('%g' % v for v in sp),
            ' '.join('[%d,0x%x]' % (t, m) for t, m in c['input']), ' '.join(seq), c['notes'].replace('|', '/')))


def worldvel(paths):
    """Traces written before chart.c converted +0x2A0 to world (vx / vy = local x30, no var_lvx): rewrite vx / vy as the
    world velocity (local x = left, y = -forward) and keep the local values as var_lvx / var_lvy (per 30 Hz frame)."""
    for p in paths:
        lines = open(p).read().split('\n')
        if len(lines) < 2 or 'var_lvx=' in lines[1]:
            continue
        out = [lines[0]]
        for line in lines[1:]:
            c = line.split('\t')
            if len(c) < 13 or not c[0].lstrip('-').isdigit():
                out.append(line)
                continue
            lx, ly, psi = float(c[8]), float(c[9]), math.radians(float(c[11]))
            fx, fy = math.cos(psi), math.sin(psi)
            c[8], c[9] = '%.1f' % (-ly * fx - lx * fy), '%.1f' % (-ly * fy + lx * fx)
            c.append('var_lvx=%.2f' % (lx / 30))
            c.append('var_lvy=%.2f' % (ly / 30))
            out.append('\t'.join(c))
        open(p, 'w').write('\n'.join(out))


if __name__ == '__main__':
    if sys.argv[1] == 'write':
        write(sys.argv[2:])
    elif sys.argv[1] == 'index':
        index(sys.argv[2])
    elif sys.argv[1] == 'worldvel':
        worldvel(sys.argv[2:])
    else:
        summary(sys.argv[2:])
