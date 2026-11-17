import sys, numpy as np
kx=np.array([0.0001,0.0005,-0.0014,-0.0016]); ky=np.array([0.0003,0,-0.0014,0.0002]); sp=np.array([0.7,1,0.7,0.3]); hh=np.array([100,75,50,25.])
top=-18895.82
rows=[]
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)>5 and p[1]=='pos': rows.append((float(p[0])/1000,float(p[2]),float(p[3]),float(p[4]),float(p[-1]),int(p[p.index('rq')+1])))
r=np.array([q for q in rows if q[4]>-1e6])
t,x,y,z,sf,rq=r.T
def surf(T,x,y): return top+ (hh*(np.cos(np.outer(x,kx)+np.outer(y,ky)+np.outer(T,sp))-1)).sum(1)
# fit T0 to recorded sf
best=min(((np.abs(surf(T0+t,x,y)-sf).mean(),T0) for T0 in np.arange(0,700,0.01)))
print('T0',best)
T0=best[1]
m=(rq==192)&(t>4)
for lag in np.arange(-4,4,0.05):
    pass
res=[]
for lag in np.arange(-4,4.001,0.02):
    d=z[m]+44.1-surf(T0+t[m]-lag,x[m],y[m]); res.append((np.sqrt(np.mean((d-d.mean())**2)),lag,d.mean()))
res.sort(); print('time lag fit', res[:3])
for name,(dx,dy) in {'world':(0,0),'inst-local':(4626.29,-34824.59),'ours':(-12446.2,-59357.3),'recA0':(12276.1,-52889.7)}.items():
    d=z[m]+44.1-surf(T0+t[m],x[m]+dx,y[m]+dy); print(name, 'rms', np.sqrt(np.mean((d-d.mean())**2)), 'mean', d.mean())
# free search for a phase offset per lane is too much; search dx,dy coarse
best=(1e9,)
for dx in np.arange(-60000,60000,250):
    for dy in [0]:
        pass
