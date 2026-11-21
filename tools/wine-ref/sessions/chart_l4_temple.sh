#!/bin/sh
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
HOLD=${HOLD:-60}
F=ledge
FL=34.80,-2417.44,-18205,1.5708; CX=1,0
ccase $F t1155_stand_jump_grab $FL 200 $CX "floor 300 west of 1155's west face, standing jump at 10: grab, hang" 20:10:14
ccase $F t1155_grab_pullup $FL 320 $CX "grab 1155, then Up 100..130 + Jump 106..112: pull up onto the block" 20:10:14 26:100:130 20:106:112
F=drop
TOP=404.6,-2416.6,-17381.4,1.5708
ccase $F t1155_top_back_run $TOP 260 $CX "on top 70 inside the west face facing +x: Down held 20..200, turns and runs off the west face: safety drop (M03)" 28:20:200
ccase $F t1155_top_walk_back $TOP 400 $CX "on top: Walk (Alt) + Down held 20..380: walks off the west face (M03 walking safety drop)" a4:20:380 28:20:380
ccase $F t1155_top_run_left $TOP 300 $CX "on top: Left held 20..200: runs ~580 along +y off the north face (running safety drop, S11 hold)" 25:20:200
ccase $F t1155_top_sprint_right $TOP 300 $CX "on top: Right + Shift held 20..200: sprints along -y off the south face" 27:20:200 a0:20:200
ccase $F t1155_top_crouch_back $TOP 300 $CX "on top: Crouch held 20..200 + Down 40..200: crouch-walk back off the west face (drop to hang?)" 11:20:200 28:40:200
ccase $F t1155_top_walk_left_noledge $TOP 400 $CX "on top: Walk + Left held 20..380: walks off the north face" a4:20:380 25:20:380
F=runjump
RW=-4400.9,-3776.14,-22714.71,3.14159; CY=0,1
for J in 50 55 60 65 70 75 80 85 90; do
    ccase $F t1204_run_jump_j$J $RW 220 $CY "runway 2400 short of ledge 1204's line, Up held 10..$((J+30)), Jump at $J..$((J+6)): catch or miss; take-off distance from px/py at the S281 row" 26:10:$((J+30)) 20:$J:$((J+6))
done
ccase $F t1204_stand_jump_far "-4400.9,-1800,-22714.71,3.14159" 200 $CY "standing jump 424 short of ledge 1204 (M13 standing catch limit 424)" 20:10:14
ccase $F t1204_stick_jump "-4400.9,-2100,-22714.71,3.14159" 200 $CY "stick jump (Up + Jump at 10) 724 short of ledge 1204" 26:10:60 20:10:14
F=pillar
PF=-2659.80,-1512.94,-22230,3.14159
ccase $F t1194_attach_ground $PF 200 $CY "200 south of pillar 1194's axis facing it, Jump at 10: attach from the floor (pillar_attach_ground)" 20:10:14
ccase $F t1194_climb_up $PF 400 $CY "attach, then Up held 100..300: climb (312 units/s)" 20:10:14 26:100:300
ccase $F t1194_slide_down $PF 420 $CY "attach, Up 100..250, then Down 300..400: slide down" 20:10:14 26:100:250 28:300:400
ccase $F t1194_round_left $PF 360 $CY "attach, Left held 100..300: round the shaft" 20:10:14 25:100:300
ccase $F t1194_jump_back $PF 360 $CY "attach, Up 100..200, then Down + Jump at 250: jump off backwards (q48)" 20:10:14 26:100:200 28:240:300 20:250:256
ccase $F t1194_walk_into $PF 200 $CY "200 south of the axis, Up held 10..120: walk / run into the pillar" 26:10:120
