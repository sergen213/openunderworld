import numpy as np, sys
def load(fn,key):
    rows=[l.split() for l in open(fn) if ' pos ' in l]
    t=np.array([float(r[0]) for r in rows]); z=np.array([float(r[4]) for r in rows]); vz=np.array([float(r[10]) for r in rows]); rq=np.array([int(r[r.index('rq')+1]) for r in rows]); sf=np.array([float(r[-1]) for r in rows])
    return t,z,vz,rq,sf
def model(z0,v0,surf,n,swan=False):
    # z0 = z at the state's first sample (already moved by the plain tick), v0 per frame; tick 2 onward
    z=z0; v=v0; out=[z]
    for k in range(2,n+2):
        if swan:
            if k<=12: z-=50
        else:
            if z>surf: z+=max(surf-z,-15)
            dl=surf-44.1-z; z+= min(15,dl) if dl>=0 else max(dl,-41.0865)
        vn=v*0.875; z+=(v+vn)/2*0.5; v=vn; out.append(z)
    return np.array(out)
for fn,key,surfcol in [('d2.txt',895,'fit'),('h3000.txt',895,'sf'),('h6000.txt',895,'sf'),('rj1.txt',895,'sf'),('h300.txt',895,'sf')]:
    t,z,vz,rq,sf=load(fn,key)
    i0=np.argmax(rq==key)
    if surfcol=='fit': S=-19099.5
    else: S=sf[i0]
    # dedupe samples: keep ones where vz changed (one per game frame)
    idx=[i0]+[i for i in range(i0+1,len(t)) if vz[i]!=vz[i-1]][:80]
    meas=z[idx]; m=model(z[i0],vz[i0],S,len(idx)-1)
    k=np.argmin(meas); print(fn,'meas min %.1f (%.1f under surf) model min %.1f  maxabs diff first 60 %.1f'%(meas.min(),S-meas.min(),m.min(),np.abs(meas[:60]-m[:60]).max()))
