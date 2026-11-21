#!/bin/sh
# Status 2026-11-21: the 1204 running-jump sweep and the 1155 air grab / pull up are in the corpus. Pillar 1194: a first
# run (no SETTLE) attached by walking in (S1186 pillar_attach_ground, S1202 idle, S1198 climb, S1219 / S1220 slide,
# S1208 / S1217 jump back, S1196 round), but those traces were overwritten by re-runs with SETTLE in which Forward
# (Up) never moved her from the spawn while the other keys did [U]; the pillar family is empty until re-recorded.
# Chart reference corpus, l4_temple (goto unit: tru = ours + (500.70, 9432.76, -19909.81), docs/REFERENCE_COMPARISON.md).
#   ledge 1155: block west face x 334.8 (tru), ledge z -17381.41, floor -18211.21; hang root off the west face
#     (173.65, y, -17875.24); on top after the pull up (404.6, -2416.6) facing +x (tru yaw pi/2). The safety-drop session
#     (l4_temple_safety_drop.sh) is the same geometry: north face y -1836.0, south face y -2998.8.
#   ledge 1204: the runway x -4400.9 along +y (tru yaw pi), ledge line y -1376.14, floor -22714.71, ledge z -21932.51
#     (runjump2/rj2.sh): running jumps from 2400 short with the press at tick J: the rush / re-aimed arc switch (M13,
#     OU-PLY-005: tru.exe 614..649 by the old wall-clock traces).
#   pillar 1194: axis (-2659.80, -1312.94) (polefid2/pil2.txt), floor ~ -22264.
# Usage (session dir exported): sessions/chart_l4_temple.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_temple; OFF=500.70,9432.76,-19909.81
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}; SETTLE=${SETTLE:-90}   # tick 0 = the idle after the landing from the pin
part() { case " ${PARTS:-ledge runjump pillar} " in *" $1 "*) return 0;; esac; return 1; }
if part ledge; then
# Pinned 600 over 1155's hang root, 6 out from it: the fall's air grab takes the ledge (REFERENCE_COMPARISON "Coordinates");
# the pull up puts her on the block facing +x, from where the drop cases leave by each face.
F=ledge; H=180,-2417.44,-17275.24,1.5708; CX=1,0; PU="26:120:150 20:126:130"
ccase $F t1155_fall_catch $H 200 $CX "pinned 600 over ledge 1155's hang root: falls, air grab, hang"
ccase $F t1155_pullup $H 300 $CX "catch, Up 120..150 + Jump 126..130: pull up (S456), on top at 175, slips off the edge (S1037) and catches 1155 again (S533)" $PU
# (Drop cases off 1155's top: from this placement the pull up ends at the block's edge, she slips off (S1037) and
# catches 1155 again (S533), so the drops are recorded off ledge 973 in chart_bedroom.sh instead.)
fi
if part runjump; then
F=runjump
RW=-4400.9,-3776.14,-22714.71,3.14159; CY=0,1
for J in 50 55 60 65 70 75 80 85 90; do
    ccase $F t1204_run_jump_j$J $RW 220 $CY "runway 2400 short of ledge 1204's line, Up held 10..$((J+30)), Jump at $J..$((J+6)): catch or miss; take-off distance from px/py at the S281 row" 26:10:$((J+30)) 20:$J:$((J+6))
done
ccase $F t1204_stand_jump_far "-4400.9,-1800,-22714.71,3.14159" 200 $CY "standing jump 424 short of ledge 1204 (M13 standing catch limit 424)" 20:10:14
ccase $F t1204_stick_jump "-4400.9,-2100,-22714.71,3.14159" 200 $CY "stick jump (Up + Jump at 10) 724 short of ledge 1204" 26:10:60 20:10:14
fi
if part pillar; then
# Keys from tick 100: with SETTLE, keys at tick 10 were not taken in the 2026-11-21 re-run (no motion at all) [U].
F=pillar; RESET=260; RESETKEYS="28:5:220 11:225:229"   # a pillar hold survives the teleport: slide down and let go first
PF=-2659.80,-1512.94,-22230,3.14159
ccase $F t1194_climb_up $PF 400 $CY "Up held 100..340: walks into pillar 1194, attaches (pillar_attach_ground), climbs (312 units/s)" 26:100:340
ccase $F t1194_slide_down $PF 460 $CY "Up 100..260 (attach, climb), then Down 300..420: slide down" 26:100:260 28:300:420
ccase $F t1194_round_left $PF 400 $CY "Up 100..160 walks into the pillar (attach), then Left held 200..380: round the shaft" 26:100:160 25:200:380
ccase $F t1194_jump_back $PF 400 $CY "Up 100..240 (attach, climb), then Down 280..340 + Jump 290..294: jump off backwards (q48)" 26:100:240 28:280:340 20:290:294
ccase $F t1194_walk_into $PF 260 $CY "200 south of the axis, Up held 100..220: walk / run into the pillar" 26:100:220
fi
