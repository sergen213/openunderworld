#!/usr/bin/env python3
# Reduce an apitrace D3D9 trace of tru.exe (sessions/render_capture.sh) to the pass list of single frames.
#   d3d9_frames.py index TRACE              resources (textures / surfaces / shaders with apitrace's disassembly:
#                                           creation call, size, format, usage) and the call number of every Present
#                                           -> TRACE.index.json
#   d3d9_frames.py frame TRACE [-k K]       the K-th last complete frame (default 3): every draw with its render
#                                           target, depth surface and state, grouped into runs of equal pass state
#                                           (DETAIL=1: every draw with its shaders, textures and VS c3.w)
#   d3d9_frames.py sort TRACE [-k K]        the translucent list in draw order with the view depth of each draw's
#                                           object origin (VS c3.w, the WorldViewProject column of the translation)
#   d3d9_frames.py shader TRACE HANDLE|CALL the disassembly of one shader (handle as printed by `frame`)
#   d3d9_frames.py lod TRACE N SUM,SUM,...  the instances among the last N frames whose draws total one of the given
#                                           primitive counts (a model's LOD batch totals), with their view distance
# Needs the Linux apitrace 14 command line (`apitrace dump`, APITRACE or PATH). Surfaces are named by what created
# them: RT0 (the device's colour surface), DS0 (its depth surface), T<call>.<level> (texture surface), Re<call>
# (CreateRenderTarget); textures in `tex` are given by their creation call.
import collections, json, os, re, subprocess, sys

APITRACE = os.environ.get('APITRACE', 'apitrace')
LINE = re.compile(r'^(\d+) (?:(\w+)::)?(\w+)\((.*)\)(?: = (.*))?$')


def dump(trace, calls=None, grep=None):
    """Calls of `apitrace dump` as (number, class, method, args, result); `grep` (an ERE) filters the text first,
    which keeps a whole-trace pass (tens of millions of calls) in grep instead of Python."""
    cmd = [APITRACE, 'dump', '--color=never', '--multiline=false']
    if calls: cmd.append('--calls=' + calls)
    cmd.append(trace)
    src = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    p = subprocess.Popen(['grep', '-a', '-E', grep], stdin=src.stdout, stdout=subprocess.PIPE) if grep else src
    for ln in p.stdout:
        m = LINE.match(ln.decode('utf-8', 'replace').rstrip('\n'))
        if m: yield int(m[1]), m[2] or '', m[3], m[4], m[5]
    p.wait(); src.wait()

def split_args(s):
    """`a = 1, b = {2, 3}, c = "x, y"` -> {'a': '1', 'b': '{2, 3}', 'c': '"x, y"'} (shader text is a quoted string)."""
    out, depth, cur, quoted, esc = {}, 0, '', False, False
    parts = []
    for ch in s:
        if quoted:
            cur += ch
            if esc: esc = False
            elif ch == '\\': esc = True
            elif ch == '"': quoted = False
            continue
        if ch == '"': quoted = True
        elif ch in '{([': depth += 1
        elif ch in '})]': depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur); cur = ''
        else:
            cur += ch
    if cur.strip(): parts.append(cur)
    for p in parts:
        k, _, v = p.strip().partition(' = ')
        out[k] = v
    return out


def ptr(v):  # "0x1234" or "&0x1234" -> "0x1234"
    m = re.search(r'0x[0-9a-f]+', v or '')
    return m[0] if m else (v or '').strip()


def floats(v):
    return [float(x) for x in re.findall(r'-?\d+(?:\.\d+)?(?:e[-+]?\d+)?', v or '')]


CREATE = re.compile(r'Create(Texture|CubeTexture|VolumeTexture|RenderTarget|DepthStencilSurface|VertexShader|PixelShader|'
                    r'OffscreenPlainSurface|VertexBuffer|IndexBuffer)$')


def index(trace):
    res, presents, alias = {}, [], {}
    pat = r'::(Present|Create[A-Za-z]+|GetSurfaceLevel|GetCubeMapSurface|GetBackBuffer|GetRenderTarget|GetDepthStencilSurface)\('
    for n, cls, fn, args, ret in dump(trace, grep=pat):
        if fn == 'Present':
            presents.append(n); continue
        if not (CREATE.search(fn) or fn in ('GetSurfaceLevel', 'GetCubeMapSurface', 'GetBackBuffer', 'GetRenderTarget',
                                             'GetDepthStencilSurface')):
            continue
        a = split_args(args)
        out = [ptr(v) for k, v in a.items() if k.startswith('pp')]
        h = out[0] if out else ''
        if fn == 'GetSurfaceLevel':
            alias[h] = {'kind': 'surface', 'of': ptr(a.get('this')), 'level': a.get('Level'), 'call': n}
        elif fn == 'GetCubeMapSurface':
            alias[h] = {'kind': 'surface', 'of': ptr(a.get('this')), 'face': a.get('FaceType'), 'level': a.get('Level'), 'call': n}
        elif fn == 'GetBackBuffer':
            alias[h] = {'kind': 'BB', 'call': n}
        elif fn in ('GetDepthStencilSurface', 'GetRenderTarget'):
            if h not in res and h not in alias: alias[h] = {'kind': 'DS0' if fn[3] == 'D' else 'RT0', 'call': n}
        elif h:
            res[h] = {'kind': fn[6:], 'call': n, **{k: v for k, v in a.items() if k in (
                'Width', 'Height', 'Levels', 'Usage', 'Format', 'Pool', 'EdgeLength', 'MultiSample')}}
            if fn.endswith('Shader'):
                res[h]['asm'] = a.get('pFunction', '').strip('"').replace('\\n', '\n')
    json.dump({'res': res, 'alias': alias, 'presents': presents}, open(trace + '.index.json', 'w'))
    print(f'{len(presents)} frames, {len(res)} resources, {len(alias)} surface handles')


def load_index(trace):
    return json.load(open(trace + '.index.json'))


def name(h, idx):
    a = idx['alias'].get(h)
    if a:
        if a['kind'] == 'surface':
            t = idx['res'].get(a['of'], {})
            return f"T{t.get('call', '?')}.{a.get('level')}" + (f"/{a['face']}" if 'face' in a else '') + \
                   f"[{t.get('Width', t.get('EdgeLength', '?'))}x{t.get('Height', '')} {t.get('Format', '').replace('D3DFMT_', '')}]"
        return a['kind']
    r = idx['res'].get(h)
    if r: return f"{r['kind'][:2]}{r['call']}[{r.get('Width', '?')}x{r.get('Height', '?')} {r.get('Format', '').replace('D3DFMT_', '')}]"
    return 'null' if h in ('NULL', '0x0', '') else h


RS = {'D3DRS_ZENABLE': 'z', 'D3DRS_ZFUNC': 'zf', 'D3DRS_ZWRITEENABLE': 'zw', 'D3DRS_ALPHABLENDENABLE': 'ab',
      'D3DRS_SRCBLEND': 'sb', 'D3DRS_DESTBLEND': 'db', 'D3DRS_BLENDOP': 'bo', 'D3DRS_COLORWRITEENABLE': 'cw',
      'D3DRS_ALPHATESTENABLE': 'at', 'D3DRS_ALPHAREF': 'ar', 'D3DRS_CULLMODE': 'cull', 'D3DRS_STENCILENABLE': 'st',
      'D3DRS_SEPARATEALPHABLENDENABLE': 'sab', 'D3DRS_SRCBLENDALPHA': 'sba', 'D3DRS_DESTBLENDALPHA': 'dba',
      'D3DRS_SCISSORTESTENABLE': 'sc', 'D3DRS_DEPTHBIAS': 'bias', 'D3DRS_SLOPESCALEDEPTHBIAS': 'sbias',
      'D3DRS_CLIPPLANEENABLE': 'clip', 'D3DRS_STENCILFUNC': 'sf', 'D3DRS_STENCILPASS': 'sp', 'D3DRS_STENCILREF': 'sr',
      'D3DRS_FOGENABLE': 'fog', 'D3DRS_SRGBWRITEENABLE': 'srgbw'}
SHORT = lambda v: re.sub(r'^D3D(CMP|BLEND|CULL|BLENDOP|STENCILOP)_', '', v)
PASSKEY = ('rt', 'ds', 'z', 'zf', 'zw', 'ab', 'sb', 'db', 'cw', 'st', 'cull')


def frames(idx, k, span=1):
    p = idx['presents']
    if len(p) < k + span: sys.exit(f'only {len(p)} frames')
    return p[-k - span] + 1, p[-k]


def walk(trace, idx, k, span=1):
    """Replay the state of the selected frame(s) (`span` frames ending with the k-th last). The state at the start is
    only what earlier calls set, so the replay starts LOOKBACK (8) frames earlier; a state still shown as `-` was not
    set in that window."""
    a0, a1 = frames(idx, k, span)
    p = idx['presents']
    s0 = p[max(0, len(p) - k - span - int(os.environ.get('LOOKBACK', '8')))]
    st = {'rt': '?', 'ds': '?', 'vs': '?', 'ps': '?'}
    tex, vsc, events = {}, {}, []
    for n, cls, fn, args, ret in dump(trace, f'{s0}-{a1}'):
        a = split_args(args)
        live = n >= a0
        if fn == 'SetRenderTarget':
            if a.get('RenderTargetIndex', '0') == '0': st['rt'] = name(ptr(a.get('pRenderTarget')), idx)
            else: st['rt%s' % a['RenderTargetIndex']] = name(ptr(a.get('pRenderTarget')), idx)
            if live: events.append((n, 'SetRenderTarget', a.get('RenderTargetIndex'), name(ptr(a.get('pRenderTarget')), idx)))
        elif fn == 'SetDepthStencilSurface':
            st['ds'] = name(ptr(a.get('pNewZStencil')), idx)
            if live: events.append((n, 'SetDepthStencilSurface', st['ds']))
        elif fn == 'SetRenderState':
            key = RS.get(a.get('State'))
            if key: st[key] = SHORT(a.get('Value', ''))
        elif fn == 'SetTexture':
            tex[int(a.get('Stage', '0'))] = ptr(a.get('pTexture'))
        elif fn == 'SetVertexShader':
            st['vs'] = ptr(a.get('pShader'))
        elif fn == 'SetPixelShader':
            st['ps'] = ptr(a.get('pShader'))
        elif fn == 'SetVertexShaderConstantF':
            r0 = int(a.get('StartRegister', '0')); f = floats(a.get('pConstantData'))
            for i in range(len(f) // 4): vsc[r0 + i] = f[4 * i:4 * i + 4]
        elif live and fn in ('Clear', 'StretchRect', 'BeginScene', 'EndScene', 'Present', 'UpdateSurface',
                             'GetRenderTargetData', 'ColorFill'):
            if fn == 'Clear':
                events.append((n, 'Clear', a.get('Flags'), a.get('Color'), a.get('Z'), a.get('Stencil'), st['rt'], st['ds']))
            elif fn == 'StretchRect':
                events.append((n, 'StretchRect', name(ptr(a.get('pSourceSurface')), idx), '->',
                               name(ptr(a.get('pDestSurface')), idx), a.get('Filter')))
            else:
                events.append((n, fn))
        elif live and fn.startswith('Draw'):
            prim = a.get('primCount') or a.get('PrimitiveCount')
            t = {s: idx['res'].get(h, {}).get('call', h) for s, h in sorted(tex.items()) if h not in ('NULL', '0x0', '')}
            events.append((n, 'draw', fn.replace('Primitive', 'P'), prim, dict(st), t, vsc.get(3, [0, 0, 0, 0])[3],
                           list(vsc.get(3, [0, 0, 0, 0]))))
    return events


def runs(events):
    out, cur = [], None
    for e in events:
        if e[1] != 'draw':
            if cur: out.append(cur); cur = None
            out.append(e); continue
        key = tuple(e[4].get(k, '-') for k in PASSKEY)
        if cur and cur['key'] == key:
            cur['draws'].append(e)
        else:
            if cur: out.append(cur)
            cur = {'key': key, 'draws': [e]}
    if cur: out.append(cur)
    return out


def show_frame(trace, k):
    idx = load_index(trace)
    a0, a1 = frames(idx, k)
    ev = walk(trace, idx, k)
    print(f'frame calls {a0}..{a1}, {sum(1 for e in ev if e[1] == "draw")} draws')
    for r in runs(ev):
        if isinstance(r, dict):
            d = r['draws']; s = d[0][4]
            vs = sorted({x[4]['vs'] for x in d}); ps = sorted({x[4]['ps'] for x in d})
            print(f"  {d[0][0]:>9}..{d[-1][0]:<9} {len(d):4} draws {sum(int(x[3] or 0) for x in d):7} prims  "
                  + ' '.join(f'{k}={v}' for k, v in zip(PASSKEY, r['key']))
                  + f" at={s.get('at', '-')} sab={s.get('sab', '-')} | vs {len(vs)} ps {len(ps)}")
            if len(d) <= 4 or os.environ.get('DETAIL'):
                for x in d:
                    print(f"      {x[0]} {x[2]} {x[3]} prims vs {idx['res'].get(x[4]['vs'], {}).get('call', x[4]['vs'])}"
                          f" ps {idx['res'].get(x[4]['ps'], {}).get('call', x[4]['ps'])} tex {x[5]} c3.w {x[6]:.1f}")
        else:
            print('  ' + ' '.join(str(x) for x in r))


def show_sort(trace, k):
    """The translucent list: every blended, depth-tested, non-depth-writing draw into the frame's main colour target
    that does not write alpha only, in draw order, with the view depth of its object origin (VS c3.w)."""
    idx = load_index(trace)
    ev = [e for e in walk(trace, idx, k) if e[1] == 'draw']
    main = collections.Counter(e[4]['rt'] for e in ev if e[4].get('zf') == 'EQUAL').most_common(1)
    main = main[0][0] if main else 'RT0'
    tl = [e for e in ev if e[4]['rt'] == main and e[4].get('ab') == 'TRUE' and e[4].get('zw') == 'FALSE'
          and e[4].get('z') != 'D3DZB_FALSE' and e[4].get('cw') != '8']
    w = [round(e[6], 1) for e in tl]
    inc = [(tl[i - 1][0], tl[i][0], w[i - 1], w[i]) for i in range(1, len(w)) if w[i] > w[i - 1] + 1e-3]
    print(f'{len(tl)} translucent draws into {main}; {len(inc)} increases of origin depth (descending = back to front)')
    print('w =', w)
    for a, b, wa, wb in inc: print(f'  increase {a} -> {b}: {wa} -> {wb}')


def show_lod(trace, n, sums):
    """Batch LODs over the last `n` frames: per frame, colour target and depth function, the draws of one instance
    (same VS c3 = WorldViewProject translation column) summed; instances whose primitive total is one of `sums`
    (e.g. a model's three LOD batch totals) are printed with the view-space distance of their origin, from c3 = clip
    position of the origin: (x / P00, y / P11, w) with the projection of a FOVY (45) degree, 16:9 view."""
    import math
    idx = load_index(trace)
    want = {int(s) for s in sums.split(',')}
    p11 = 1 / math.tan(math.radians(float(os.environ.get('FOVY', '45'))) / 2); p00 = p11 * 9 / 16
    frame, inst = 0, collections.defaultdict(int)
    rows = []
    def flush():
        for (rt, zf, c3), s in inst.items():
            if s in want:
                d = math.sqrt((c3[0] / p00) ** 2 + (c3[1] / p11) ** 2 + c3[3] ** 2)
                rows.append((frame, rt, zf, s, c3[3], d))
        inst.clear()
    for e in walk(trace, idx, 3, n):
        if e[1] == 'Present':
            flush(); frame += 1
        elif e[1] == 'draw':
            inst[(e[4]['rt'], e[4].get('zf'), tuple(round(v, 2) for v in e[7]))] += int(e[3] or 0)
    flush()
    for r in rows: print('frame %d rt %s zf %s: %d prims, origin view depth %.1f, distance %.1f' % r)


def show_shader(trace, key):
    res = load_index(trace)['res']
    for h, r in res.items():
        if (h == key or str(r['call']) == key) and 'asm' in r:
            print(f"{h} created at call {r['call']}\n{r['asm']}")


if __name__ == '__main__':
    cmd, trace = sys.argv[1], sys.argv[2]
    k = int(sys.argv[sys.argv.index('-k') + 1]) if '-k' in sys.argv else 3
    {'index': lambda: index(trace), 'frame': lambda: show_frame(trace, k), 'sort': lambda: show_sort(trace, k),
     'shader': lambda: show_shader(trace, sys.argv[3]), 'lod': lambda: show_lod(trace, int(sys.argv[3]), sys.argv[4])}[cmd]()
