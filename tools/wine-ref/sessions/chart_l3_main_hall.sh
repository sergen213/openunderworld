#!/bin/sh
# Chart reference corpus, l3_main_hall (goto unit: tru = ours + (1583.82, 1911.28, -1939.49), docs/REFERENCE_COMPARISON.md).
# Pole 948 (axis z 1861 ours) -> ledge 946 on a platform 1052 ahead (GameReal.MainHallPoleFlingLandsOnPlatform;
# polefid2/mhsw.txt started from tru (2626.38, -2000, -708.52), the idle hang's root): pinned 250 over it she falls
# and air-grabs the pole, facing +x (tru yaw pi/2, camera (1,0)).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l3_main_hall.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_main_hall; OFF=1583.82,1911.28,-1939.49
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}
F=pole
P=2626.38,-2000,-458.5,1.5708; CP=1,0
ccase $F mh_pole_airgrab $P 200 $CP "pinned 250 over pole 948's idle hang root: falls, air grab"
ccase $F mh_pole_fling_platform $P 360 $CP "air grab, Up 120..260 + Jump 150..154: fling onto platform 946 (35.87 + 74.1 a frame, 0.783 s, M07)" 26:120:260 20:150:154
ccase $F mh_pole_jump_neutral $P 300 $CP "air grab, Jump 150..154 with no stick" 20:150:154
