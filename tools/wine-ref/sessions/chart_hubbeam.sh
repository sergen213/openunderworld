#!/bin/sh
# SPAWN-free balance-beam walk (Main / ChartB / ChartBlend): l2_hubhall checkpoint 3099 (ours (-17052.4, 32736.0, 7417.8)
# facing -x) stands 397 from beam 759's east end (ours x -17437.3..-18404.1, y 32637.3, z 7433.3; our collision: flat
# floor at 7433.3 straight to it, ours/floorline.cpp; survey of every type-14 markup against docs/CHECKPOINTS.md
# 2026-11-22). Lara is born there at a fresh load (chart_birth.sh method) and walks onto the beam by keys, camera pinned
# behind her (-x). One take (CAMDUMP): Up 30..330 (walk -x onto and along the beam), Left 350..470, Right 490..610,
# Down 630..750 (camera-relative), 800 ticks. Fields as chart_beam.sh (ANIM, stick +0x8 / +0x18, layer, weights, the
# chart agent's beam controller A+0x28 / +0x358 / +0x35C / +0x3A0 / +0x3A4 / +0x3A8 / +0x3B4, player data).
# Writes beam/h759_walk_sweep_birth.tsv + case.json.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_hubbeam.sh
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
W="$WREF_SRC/w.sh"
# 2026-11-22 run 1: with the camera straight down -x she walked 99 beside the beam line (y 32736 vs 32637) and fell off
# the platform at x -17476 (S11, then a catch S542); the camera now points from the checkpoint at the beam's east end
# (-17437.3, 32637.3): direction (-0.9685, -0.2490)
UNIT=l2_hubhall; U=3099; CF=-0.9685,-0.2490,1264,300
cunit l3_main_bedroom || exit 1
i=0; until "$W" wmem state | tr -d '\r' | grep -q " flags 00000000 " || [ $i -ge 60 ]; do sleep 5; i=$((i+1)); done
P=$("$W" wmem rd C59264 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'); UX=$(printf %X $U)
wl() { "$W" wmem w32 "$(printf %X $((0x$P + 0x90)))" "$UX" > /dev/null; "$W" wmem w32 "$(printf %X $((0x$P + 0x114)))" "$UX" > /dev/null; }
wl; "$W" wmem goto $UNIT
i=0; while [ $i -lt 40 ]; do wl; sleep 0.5; i=$((i+1)); done
i=0; until "$W" wmem state | tr -d '\r' | grep -qi "unit $UNIT .*flags 00000000" || [ $i -ge 30 ]; do sleep 10; i=$((i+1)); done
sleep 60; "$W" wmem state | tr -d '\r'
cmeasure $UNIT || exit 1
CAMFOLLOW=$CF "$W" chart trace 60 > /dev/null
export ANIM=1 VARS=lf4:L:F4,lf8:L:F8,l8c:L:8C:f,l90:L:90:f,l94:L:94:f,w0:w:0:f,w1:w:4:f,w2:w:8:f,w3:w:C:f,w4:w:10:f,w5:w:14:f,\
k28:k:28:f,k50:k:50:f,s08:s:8:f,s18:s:18:f,a28:c:28:f,a358:c:358:f,a35c:c:35C:f,a3a0:c:3A0:f,a3a4:c:3A4:f,a3a8:c:3A8:f,\
a3b4:c:3B4:f,c94:d:C94,c98:d:C98:f,ed0:d:ED0,cc0:d:CC0:f,cc4:d:CC4:f,cc8:d:CC8:f,s1b0:S:1B0:f,s1b4:S:1B4:f,s1b8:S:1B8:f,s1c0:S:1C0
d="$OU_CHART_DIR/beam"
if [ "$1" = sweep ]; then # ChartB: Up 30..150 (mount at ~55, stops well before the far end), then on the beam
    n=h759_dir_sweep_birth; T=600; K="26:30:150 25:170:260 27:280:370 28:390:480 26:500:590"
    NO="Up 30..150 walks onto the beam, then on the beam Left 170..260, Right 280..370, Down 390..480, Up 500..590 (camera-relative)"
elif [ "$1" = watch8c ]; then # ChartB / ChartBlend: who writes layer +0x8C in S935: the five functions that store +0x8C
    # and touch +0xF9 (tru_dis), each with the layer +0x80..+0x8F copied at entry and +0x8C.. at return (:Y)
    n=h759_watch8c_birth; T=220; K="26:30:220"; NO="Up 30..220 walks onto and along the beam; call log of the +0x8C writers"
    export CALLS=1 CALLFN=4336C0:L=5:Y+80:RY+80,7148B0:Y+80:RY+80,72A510:L=6:Y+80:RY+80,742770:Y+80:RY+80,76B8F0:L=5:Y+80:RY+80
else
    n=h759_walk_sweep_birth; T=800; K="26:30:330 25:350:470 27:490:610 28:630:750"
    NO="Up 30..330 walks onto and along the beam (far end at ~280: S893, drop to a hang), then Left 350..470, Right 490..610, Down 630..750 (camera-relative, in the hang)"
fi
CAMDUMP=1 CAMFOLLOW=$CF OFFSET=$OFF "$W" chart trace $T $K | tr -d '\r' > "$d/$n.tsv"
S=$(awk -F'\t' 'NR==2{for(i=14;i<=NF;i++){if($i~/var_tpx=/)x=substr($i,9); if($i~/var_tpy=/)y=substr($i,9); if($i~/var_tpz=/)z=substr($i,9); if($i~/var_tyaw=/)w=substr($i,10)}; print x","y","z","w}' "$d/$n.tsv")
python3 "$CHART_PY" write "$d/$n.case.json" --unit $UNIT --offset "$OFF" --spawn "$S" --hold 0 --settle 0 --ticks $T --cam "$CF" \
    --notes "NO SPAWN: born on checkpoint 3099 at a fresh l2_hubhall load, 397 from beam 759's east end (spawn = her position at tick 0); camera pinned looking from the checkpoint at the beam end (-0.9685, -0.2490); $NO; CAMDUMP" -- $K
python3 - "$d/$n.case.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c['spawn']['method'] = 'checkpoint birth (uid 3099, fresh l2_hubhall load) + keys only; replays must not SPAWN'
json.dump(c, open(sys.argv[1], 'w'), indent=1)
PY
python3 "$CHART_PY" summary "$d/$n.tsv" | cut -c1-600
