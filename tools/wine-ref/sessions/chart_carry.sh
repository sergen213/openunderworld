#!/bin/sh
# Sideways carry after a teleport (ChartB): harness or game? Takes with tru.exe's position setter hooked for Lara only:
#   0x60F680(instance, float* newPos, const char* why) (CALLFN :F1=E7F094; copies newPos, instance +0x20 before the
#   call, the first 16 bytes of `why`; the return address shows the caller), 0x5D34B0(instance, float* delta, why) (the
#   delta), and the player ground check 0x5DCE20 (return address only).
#   traprooms_floor: pinned 60 ticks on the pit floor (1135.8, 105.4, -2550), then 60 free ticks (control).
#   traprooms_drop:  the 2-tick-pin drop from (6958, 4474, -500) again.
#   bedroom_floor:   pinned 60 ticks on the l3_main_bedroom floor (1137.7, -3997, -2733), then 60 free ticks.
# Before each SPAWN the instance position is logged (`before`): where the harness moves her from. Writes
# $OU_WREF_DIR/carry_<take>.tsv and a decode in $OU_WREF_DIR/carry.txt.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_carry.sh [traprooms|bedroom]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
W="$WREF_SRC/w.sh"; FN=60f680:F1=E7F094:2:1+20:3,5d34b0:F1=E7F094:2:3,5dce20
take() { # NAME SPAWN HOLD TICKS [KEYS...]
    n=$1 sp=$2 ho=$3 ti=$4; shift 4
    echo "$n before: $("$W" wmem state | tr -d '\r' | sed 's/.* pos /pos /')" >> "$OU_WREF_DIR/carry_before.txt"
    CALLS=1 CALLFN=$FN SPAWN=$sp HOLD=$ho "$W" chart trace "$ti" "$@" | tr -d '\r' > "$OU_WREF_DIR/carry_$n.tsv"
    CALLFN=$FN "$W" chart callunhook > /dev/null; }
for part in ${*:-traprooms bedroom}; do
    case $part in
    traprooms) UNIT=l4_traprooms; cunit $UNIT || exit 1; sleep 60
        echo "goto start: $("$W" wmem state | tr -d '\r')" >> "$OU_WREF_DIR/carry_before.txt"
        take traprooms_floor 1135.8,105.4,-2550,3.14159 60 60
        "$W" chart trace 30 > /dev/null
        take traprooms_drop 6958,4474,-500,3.14159 2 60
        # placement re-takes (chapel ch_hang_land method: placed at a verified spot, 2-tick pin):
        # tr_pole_drop placed 250 over pole 1336's idle hang root (6942.22, 4947.40, -165.58), grab, Crouch 120..124;
        # the floor drop placed 10 over the group-0 faces (z -939.8) at (6958, 4474)
        "$W" chart trace 30 > /dev/null
        take tr_pole_drop_placed 6942.22,4947.40,84.42,3.14159 2 260 11:120:124
        "$W" chart trace 30 > /dev/null
        take floor_placed 6958,4474,-930,3.14159 2 90 ;;
    bedroom) UNIT=l3_main_bedroom; cunit $UNIT || exit 1
        echo "new game start: $("$W" wmem state | tr -d '\r')" >> "$OU_WREF_DIR/carry_before.txt"
        take bedroom_floor 1137.7,-3997,-2733,0 60 60 ;;
    esac
done
python3 - "$OU_WREF_DIR" <<'PY' | tee "$OU_WREF_DIR/carry.txt"
import sys, struct, glob, os
f = lambda x: struct.unpack('<f', struct.pack('<I', int(x, 16)))[0]
txt = lambda ws: b''.join(struct.pack('<I', int(w, 16)) for w in ws).split(b'\0')[0].decode('latin-1')
D = sys.argv[1]; print(open(os.path.join(D, 'carry_before.txt')).read())
for path in sorted(glob.glob(os.path.join(D, 'carry_*.tsv'))):
    print('==', os.path.basename(path)); prev = None
    for line in open(path).read().split('\n')[1:]:
        if not line: continue
        c = line.split('\t'); d = {x.split('=', 1)[0]: x.split('=', 1)[1] for x in c[13:] if '=' in x}
        pos = [float(d['var_tpx']), float(d['var_tpy']), float(d['var_tpz'])]
        step = [round(pos[k] - prev[k], 2) for k in range(3)] if prev else [0, 0, 0]; prev = pos
        calls = [e for e in d.get('var_calls', '').split(';') if e]
        setters = [e for e in calls if e.startswith('60f680') or e.startswith('5d34b0')]
        print('tick %s S%s pos %s step %s  setter calls %d' % (c[0], c[1], [round(v, 2) for v in pos], step, len(setters)))
        for e in setters:
            body, fr = e.rsplit('@', 1); head, _, ret = body.partition('^'); a = head.split(':')
            if a[0] == '60f680':
                newp = [round(f(x), 2) for x in a[7:10]]; before = [round(f(x), 2) for x in a[11:14]]
                print('    60f680 ret %s why "%s" new %s before %s delta %s @%s' % (ret, txt(a[15:19]), newp, before,
                      [round(newp[k] - before[k], 2) for k in range(3)], fr))
            else:
                print('    5d34b0 ret %s why "%s" delta %s @%s' % (ret, txt(a[11:15]), [round(f(x), 2) for x in a[7:10]], fr))
PY
