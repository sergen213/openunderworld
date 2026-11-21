#!/bin/sh
# 2026-11-21: the first run's goto came while gameTracker flag 1 was still set (the prologue's opening cinematic, 10 s
# after play began) and left the game without frames; cunit now waits for the flags to clear. The offset is measured
# (cmeasure) to check the doc's; the pole placement is a tru-frame position from polefid2/mhsw.txt.
# Chart reference corpus, l3_main_hall (goto unit: tru = ours + (1583.82, 1911.28, -1939.49), docs/REFERENCE_COMPARISON.md).
# Pole 948 (axis z 1861 ours) -> ledge 946 on a platform 1052 ahead (GameReal.MainHallPoleFlingLandsOnPlatform;
# polefid2/mhsw.txt started from tru (2626.38, -2000, -708.52), the idle hang's root): pinned 250 over it she falls
# and air-grabs the pole, facing +x (tru yaw pi/2, camera (1,0)).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l3_main_hall.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_main_hall; OFF=1583.82,1911.28,-1939.49
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || echo "offset not measured: the doc's kept" >&2
HOLD=${HOLD:-60}
F=pole
P=2626.38,-2000,-458.5,1.5708; CP=1,0
ccase $F mh_pole_airgrab $P 200 $CP "pinned 250 over pole 948's idle hang root: falls, air grab"
ccase $F mh_pole_fling_platform $P 360 $CP "air grab, Up 120..260 + Jump 150..154: fling onto platform 946 (35.87 + 74.1 a frame, 0.783 s, M07)" 26:120:260 20:150:154
ccase $F mh_pole_jump_neutral $P 300 $CP "air grab, Jump 150..154 with no stick" 20:150:154
ccase $F mh_pole_swing $P 420 $CP "air grab, Up held 120..380: swing" 26:120:380
ccase $F mh_pole_top $P 360 $CP "air grab, Jump 140..144 with no stick: onto the pole's top (hpole_to_perch: pole to perch)" 20:140:144
ccase $F mh_pole_top_jump_fwd $P 480 $CP "pole top, then Up 300..340 + Jump 304..308: jump forward off the top" 20:140:144 26:300:340 20:304:308
ccase $F mh_pole_drop $P 260 $CP "air grab, Crouch 120..124: drop" 11:120:124
