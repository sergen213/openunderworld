#!/bin/sh
# Status 2026-11-21: offset measured (34604.60, 3795.87, 12744.02; 115/162), but from this spawn she falls to a floor
# 30 ticks below in tru.exe instead of reaching ladder 234: no ladder states (traces discarded) [U].
# Chart reference corpus, l1_ship_sinking_hall ladder 234 (GameReal.ShipSinkingHallLadder): in the valve wall's plane
# (y 29 ours, open side -y), 158 wide from x 1401; the shaft has no floor: she comes onto it out of the air from ours
# (1480, -120, 1100) facing +y (yaw 90) with the stick forward. The offset is measured from the instances present in
# both frames after the goto (cmeasure; the first run's "start - intro" was wrong). Camera behind her (0,1): Up = +y
# (towards the ladder) while airborne, then up the ladder.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l1_ship_sinking_hall.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l1_ship_sinking_hall
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}
L=$(tru 1480 -120 1100 90); CL=0,1; A="26:0:30"       # attach out of the air
F=ladder; RESETKEYS="11:5:9"
ccase $F sh_ladder_attach $L 220 $CL "in the air in front of ladder 234, Up 0..30: attach (ladder_attach_M), settle on a rung" $A
ccase $F sh_ladder_climb_up $L 520 $CL "attach, Up held 120..480: climb up (rungs 110.619 apart, M96)" $A 26:120:480
ccase $F sh_ladder_climb_up_top $L 1200 $CL "attach, Up held 120..1150: climb to the top and off it (ladder_detach_ground_top)" $A 26:120:1150
ccase $F sh_ladder_climb_down $L 520 $CL "attach, Down held 120..480: climb down" $A 28:120:480
ccase $F sh_ladder_climb_down_bottom $L 1200 $CL "attach, Down held 120..1150: down to the bottom rung (no floor: lean back?)" $A 28:120:1150
ccase $F sh_ladder_fast_up $L 520 $CL "attach, Up held 120..480 + Interact held 140..480: fast climb?" $A 26:120:480 45:140:480
ccase $F sh_ladder_fast_down $L 520 $CL "attach, Down held 120..480 + Interact held 140..480: slide down?" $A 28:120:480 45:140:480
ccase $F sh_ladder_jump_up $L 400 $CL "attach, Up 120..200 + Jump 150..154: jump up (q23 80.15 a frame)" $A 26:120:200 20:150:154
ccase $F sh_ladder_jump_back $L 400 $CL "attach, Jump 150..154, no stick: leap back (q22 47.41 + 60.05)" $A 20:150:154
ccase $F sh_ladder_left_hold $L 400 $CL "attach, Left held 120..360: side reach / jump antic (OU-PLY-004)" $A 25:120:360
ccase $F sh_ladder_left_jump $L 400 $CL "attach, Left 120..220 + Jump 170..174: side jump left" $A 25:120:220 20:170:174
ccase $F sh_ladder_right_jump $L 400 $CL "attach, Right 120..220 + Jump 170..174: side jump right" $A 27:120:220 20:170:174
ccase $F sh_ladder_drop $L 320 $CL "attach, Crouch 150..154: let go" $A 11:150:154
