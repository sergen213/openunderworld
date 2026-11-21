#!/bin/sh
# Chart reference corpus, l3_chapel (goto unit: tru = ours + (3817.47, -3091.84, 3143.17) (ChartCore: our 'player' intro 4928.4 200.5 -4740.8; tru start 8745.87 -2891.34 -1597.63)).
# Balance beam 1031 (63 wide, x 4708.6 ours, top z -6163) with ledge loop 1009 round its top (x 4740.6 / 4677.4)
# (GameReal.BeamFallsCatchScaffoldLedge, ChapelHangClimbsOntoBalanceBeam). Beam cases: dropped 40 onto the beam at ours
# (4708.6, 4594.8) facing -y (tru yaw 0, camera (0,-1)). Hang cases: pinned 600 over loop 1009's +x side hang root.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l3_chapel.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_chapel; OFF=3817.47,-3091.84,3143.17
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}
F=beam
B=8526.07,1502.96,-2979.8,0; CB=0,-1
ccase $F ch_beam_land $B 200 $CB "dropped 40 onto beam 1031: lands into the beam idle?"
cground $F ch_beam_walk $B 360 $CB "on the beam, Up held 60..300: bbeam_walk (140 units/s by the clip, M10)" 26:60:300
cground $F ch_beam_walk_left_fall $B 700 $CB "Up + Left held 60..660: balance, about to fall, fall to the left, catch loop 1009 (OU-PLY-001)" 26:60:660 25:60:660
cground $F ch_beam_walk_right_fall $B 700 $CB "Up + Right held 60..660: fall to the right" 26:60:660 27:60:660
cground $F ch_beam_left_hold $B 700 $CB "Left held 60..660 standing on the beam: lean / about to fall" 25:60:660
cground $F ch_beam_right_hold $B 700 $CB "Right held 60..660 standing on the beam" 27:60:660
cground $F ch_beam_left_recover $B 500 $CB "Up + Left 60..200, then Right 200..300: off balance and recover" 26:60:300 25:60:200 27:200:300
cground $F ch_beam_jump $B 300 $CB "Jump 80..84 on the beam" 20:80:84
cground $F ch_beam_run_jump $B 360 $CB "Up 60..200 + Jump 120..124 on the beam" 26:60:200 20:120:124
cground $F ch_beam_turn $B 300 $CB "Down 60..120 on the beam: turn round" 28:60:120
cground $F ch_beam_crouch $B 300 $CB "Crouch 60..64 on the beam: drop off to the hang?" 11:60:64
cground $F ch_beam_sprint $B 400 $CB "Up + Shift 60..300: beam sprint? (M10 unmeasured)" 26:60:300 a0:60:300
F=ledge
H=8724.1,1408.2,-2913.8,-1.5708
ccase $F ch_loop1009_hang $H 240 -1,0 "pinned 600 over loop 1009's +x side hang root facing -x: falls, catches the loop"
ccase $F ch_loop1009_jump_onto_beam $H 360 -1,0 "catch, Jump 150..154: bbeam_ledgeu_climbup onto the beam (S439 T698 -> S882 -> S885)" 20:150:154
ccase $F ch_loop1009_shimmy $H 400 -1,0 "catch, Right held 150..380: shimmy round the loop" 27:150:380
