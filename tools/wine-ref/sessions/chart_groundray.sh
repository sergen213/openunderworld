#!/bin/sh
# Ground-check rays during the l4_traprooms drop through the floor under pole 1336 (ChartB). CALLFN entry hooks on
# 0x5DCE20 (the ground check; its return address shows the caller: 0x661569 / 0x6A6297 / 0x71BC70) and its terrain ray
# `__cdecl 0x493330(uint flags, float* start, float* end, int* result, instance)` with start / end copied at entry and
# 8 result dwords copied on return (result[0] 0 none / 1 terrain / 2 object, [1] / [2] the mesh and its client = the
# terrain group, [4..6] the hit point). Drop from tru (6958, 4474, -500). Writes $OU_WREF_DIR/groundray.tsv and a decode
# in $OU_WREF_DIR/groundray.txt. Usage (session dir exported, see chart_lib.sh): sessions/chart_groundray.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7342.10,25801.50,-7298.09
[ "$1" = noload ] || { cunit $UNIT || exit 1; sleep 60; }
W="$WREF_SRC/w.sh"; FN=5dce20,493330:2:3:R4
"$W" chart trace 60 11:5:9 > /dev/null; sleep 10
CALLS=1 CALLFN=$FN SPAWN=6958,4474,-500,3.14159 HOLD=2 "$W" chart trace 90 | tr -d '\r' > "$OU_WREF_DIR/groundray.tsv"
CALLFN=$FN "$W" chart callunhook
python3 - "$OU_WREF_DIR/groundray.tsv" <<'PY' | tee "$OU_WREF_DIR/groundray.txt"
import sys, struct
f = lambda x: struct.unpack('<f', struct.pack('<I', int(x, 16)))[0]
for line in open(sys.argv[1]).read().split('\n')[1:]:
    if not line: continue
    c = line.split('\t'); d = {x.split('=', 1)[0]: x.split('=', 1)[1] for x in c[13:] if '=' in x}
    print('tick %s S%s tru z %s' % (c[0], c[1], d.get('var_tpz')))
    for e in d.get('var_calls', '').split(';'):
        if not e: continue
        body, fr = e.rsplit('@', 1); head, _, res = body.partition('=')
        head, _, ret = head.partition('^'); a = head.split(':')
        if a[0] == '5dce20': print('   5dce20 ret %s args %s @%s' % (ret, ' '.join(a[2:7]), fr))
        elif a[0] == '493330':
            st = [round(f(x), 1) for x in a[7:10]]; en = [round(f(x), 1) for x in a[11:14]]
            r = res.split(':') if res and res != '?' else []
            hit = ('kind %d mesh %s client %s r3 %s point %s' % (int(r[0], 16), r[1], r[2], r[3], [round(f(x), 1) for x in r[4:7]])) if r else res
            print('   493330 ret %s flags %s start %s end %s -> %s @%s' % (ret, a[2], st, en, hit, fr))
PY
