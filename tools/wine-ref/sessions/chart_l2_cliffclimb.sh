#!/bin/sh
# Status 2026-11-21: the step-941 run from ours y -2450 (d900) and the sprint case started in the sea (swim states);
# the start-minus-intro offset measured in this session differs from the doc's by (-190, -13, +246) [U].
# Chart reference corpus, l2_cliffclimb (goto unit: tru = ours + (12446.2, 59357.3, -19003.8), REFERENCE_COMPARISON M29).
# Obstacles (GameReal.CliffclimbObstaclesAndLedgeGrabs): waist-high step 941 run into from ours (-28152, -1550, 3100)
# facing +y (tru yaw pi, camera (0,1)); chest-high block 939 jumped at from ours (-28029, -650, 3300); the 614-high
# wall 911 from ours (-29439, -900, 3300) yaw 110. OU-PLY-006: where the hurdle starts on the run (query 96 reach).
# Free climb (DICE): rock hold 5 at tru (-12101.3, 56281.2, -16651.3) yaw 2.214 (runjump2/dice.sh): she falls onto it.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l2_cliffclimb.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l2_cliffclimb; OFF=12446.2,59357.3,-19003.8
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}
F=hurdle
for D in 0 300 600 900; do
    cground $F cc_step941_run_d$D "-15705.8,$(awk "BEGIN{print 57807.3 - $D}"),-15903.8,3.14159" 260 0,1 "run (Up 30..200) into step 941 from ours y $(( -1550 - D )): hurdle onset (S958 entry row: distance to the step's line)" 26:30:200
done
cground $F cc_step941_walk "-15705.8,57507.3,-15903.8,3.14159" 420 0,1 "walk (Alt + Up 30..400) into step 941 from ours y -1850" a4:30:400 26:30:400
cground $F cc_step941_sprint "-15705.8,56907.3,-15903.8,3.14159" 260 0,1 "sprint (Up + Shift 30..200) into step 941 from ours y -2450" 26:30:200 a0:30:200
cground $F cc_block939_stand_jump "-15582.8,58707.3,-15703.8,3.14159" 260 0,1 "standing jump at 30 in front of block 939 (474 high): hurdle attach mid / high, onto" 20:30:34
cground $F cc_block939_run "-15582.8,58207.3,-15703.8,3.14159" 260 0,1 "run (Up 30..200) at block 939 from 500 farther back: wall_collide_mid only (no jump)" 26:30:200
F=ledge
cground $F cc_wall911_stick_jump_climb "-16992.8,58457.3,-15703.8,3.4907" 360 -0.342,0.940 "Up 40..70 + Jump 40..44 at the 614-high wall 911, then Jump 150..154: catch, pull up" 26:40:70 20:40:44 20:150:154
F=freeclimb
DI=-12101.3,56281.2,-16651.3,2.214; CD=0.8,0.6
ccase $F cc_dice_attach $DI 200 $CD "pinned in front of rock hold 5, falls: air attach onto the free-climb hold (DICE_attach)" 
ccase $F cc_dice_up_jump $DI 360 $CD "attach, Up 100..260 + Jump 160..164: DICE jump (hands arrive 0.39 s after the press, M05)" 26:100:260 20:160:164
ccase $F cc_dice_up_hold $DI 360 $CD "attach, Up held 100..340: move / lean up" 26:100:340
ccase $F cc_dice_left $DI 360 $CD "attach, Left held 100..340: move / lean left" 25:100:340
ccase $F cc_dice_right $DI 360 $CD "attach, Right held 100..340: move / lean right" 27:100:340
ccase $F cc_dice_down $DI 360 $CD "attach, Down held 100..340: move down / lean back" 28:100:340
ccase $F cc_dice_back_jump $DI 300 $CD "attach, Down 100..200 + Jump 140..144: jump off backwards" 28:100:200 20:140:144
ccase $F cc_dice_drop $DI 260 $CD "attach, Crouch 100..104: let go" 11:100:104
