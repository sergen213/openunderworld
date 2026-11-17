import sys
def sim(z0, v0, top, surf, n=200, g=5.72443):
    # z0 root, v0 units/frame (down negative); 60 Hz ticks, tm=0.5
    z, v, st, out = z0, v0, 'fall', []
    for i in range(n):
        if st=='fall':
            z += v*0.5 - 0.5*g*0.25; v -= g*0.5
            if z < top: st='enter'
        elif st=='enter':  # tru: one more frame of plain motion on the state switch
            z += v*0.5; st='s104'
        else:
            vn = v*0.875; z += (v+vn)/2*0.5; v = vn
            if z > surf: z += max(surf - z, -15)          # 1302
            tgt = surf - 44.1; dl = tgt - z               # 1301
            z += min(15, dl) if dl >= 0 else max(dl, -6.5*3.1605/0.5)
        out.append((i, st, z, v))
    return out
if __name__ == '__main__':
    z0, v0, top, surf = map(float, sys.argv[1:5])
    o = sim(z0, v0, top, surf)
    m = min(o, key=lambda r: r[2]); print('min z %.1f (%.1f below surf) at tick %d' % (m[2], surf - m[2], m[0]))
    for r in o[:60:3]: print(r)
