#!/bin/sh
# Chart reference corpus, l2_cliffclimb: the offset re-measured from instances (cmeasure: the first run's "start -
# intro" differed from the doc's (12446.2, 59357.3, -19003.8) by (-190, -13, +246)), and swim climb-outs at ledge 936
# seg 0 (BUGS OU-PLY-003; w2/WineCheck10/sessD.sh): treading pinned 180 out from the ledge at the water surface
# (tru z -18939.8), facing it (tru yaw 1.406), camera along (0.986, -0.164); Forward from tick 60. Positions here are
# tru-frame (the WineCheck10 placements), so the cases do not depend on the offset. A and the "along" distances name the
# point along seg 0 the swim starts across from (the wall-hug notch is 48..80 along).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l2_cliffclimb_swim.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l2_cliffclimb; OFF=12446.2,59357.3,-19003.8
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || echo "offset not measured: the doc's kept" >&2
HOLD=${HOLD:-60}; RESETKEYS=""
F=swim; TZ=-18939.8; CS=0.986,-0.164
for a in "30 -11959.4 56238.5" "64 -11964.8 56205.0" "90 -11969.2 56179.3" "110 -11972.5 56159.5"; do
    set -- $a
    ccase $F cc_climbout_a$1 "$2,$3,$TZ,1.406" 360 $CS "treading 180 out from ledge 936 seg 0 at $1 along, Forward held 60..300: swim to the ledge and climb out (swim_climbout_A/_B/_C; S238 / S240 / S241 / S242 / S1253)" 26:60:300
done
ccase $F cc_climbout_sprint "-11964.8,56205.0,$TZ,1.406" 360 $CS "treading at 64 along, Forward + Shift held 60..300: sprint-swim into the climb out" 26:60:300 a0:60:300
ccase $F cc_tread_jump "-11964.8,56205.0,$TZ,1.406" 300 $CS "treading at 64 along, Forward 60..120 + Jump 100..104: climb out with a jump press" 26:60:120 20:100:104
ccase $F cc_tread_dive "-11964.8,56205.0,$TZ,1.406" 360 $CS "treading, Crouch 60..64 (dive), Forward 100..300: under water to the ledge" 11:60:64 26:100:300
