import math, sys
# turn.py trace.txt : heading (rotz - 90 deg), motion direction, camera look, per sample
rows = []
for l in open(sys.argv[1]):
    f = l.split()
    rows.append((list(map(float, f[1:4])), float(f[7]), list(map(float, f[11:14])), list(map(float, f[15:18]))))
dt = float(sys.argv[2]) if len(sys.argv) > 2 else 0.03
for i in range(1, len(rows)):
    L, r, C, P = rows[i]
    L0 = rows[i - 1][0]
    look = math.atan2(P[1] - C[1], P[0] - C[0])
    head = r - math.pi / 2
    mv = math.atan2(L[1] - L0[1], L[0] - L0[0])
    v = math.hypot(L[0] - L0[0], L[1] - L0[1]) / dt
    print("t %.2f head %7.1f move %7.1f v %6.0f look %7.1f stickdir(look-90) %7.1f head-stick %6.1f move-head %6.1f" % (
        i * dt, math.degrees(head), math.degrees(mv), v, math.degrees(look), math.degrees(look - math.pi / 2),
        math.degrees(math.remainder(head - (look - math.pi / 2), 2 * math.pi)), math.degrees(math.remainder(mv - head, 2 * math.pi))))
