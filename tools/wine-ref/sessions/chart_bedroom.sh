#!/bin/sh
# Chart reference corpus, l3_main_bedroom (new-game prologue, its own frame: tru = ours + (9622, 333, -2977)).
# Ledge 973 (the prologue wall, GameReal.HangLeapsMatchTruTraces): every hang case starts with the standing jump from
# ours (-8484.3, -4330, 244) facing the wall (tru yaw 0 = ours -90 deg; tru forward (sin yaw, -cos yaw) = -y), which
# catches the ledge ~57 ticks after the press; the action keys start at tick 100. Camera pinned behind her (0,-1).
# Needs a running session in the prologue (newgame.sh); run with the session's OU_WREF_DIR / OU_WREF_NAME exported.
# The spawn stands in a fire trigger: GOD (default) keeps her health full.
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_main_bedroom; OFF=9622,333,-2977
"$WREF_SRC/w.sh" wmem state | grep -qi "unit l3_main_bedroom" || { echo "not in l3_main_bedroom: run newgame.sh" >&2; exit 1; }
S=1137.7,-3997,-2733,0; C=0,-1; J=20:10:14; HOLD=${HOLD:-60}
F=ledge
ccase $F hang_grab $S 160 $C "standing jump at ledge 973 from ~137 short of its line: catch and settle into the hang" $J
ccase $F hang_shimmy_left $S 300 $C "hang, Left held 100..250: shimmy" $J 25:100:250
ccase $F hang_shimmy_right $S 300 $C "hang, Right held 100..250: shimmy" $J 27:100:250
ccase $F hang_shimmy_right_long $S 700 $C "hang, Right held 100..650: shimmy to the ledge's end (lean S457 -> S449 or corner)" $J 27:100:650
ccase $F hang_shimmy_left_long $S 700 $C "hang, Left held 100..650: shimmy to the ledge's other end" $J 25:100:650
ccase $F hang_hustle_left $S 260 $C "hang, Left held 100..200 + Interact 106..112: hustle (fast shimmy)" $J 25:100:200 45:106:112
ccase $F hang_pullup $S 320 $C "hang, Up 100..130 + Jump 106..112: pull up (climb onto the top)" $J 26:100:130 20:106:112
ccase $F hang_up_hold $S 300 $C "hang, Up held 100..250, no jump: lean up (S434) and release" $J 26:100:250
ccase $F hang_jump_up $S 300 $C "hang, Up 100..200 + Jump 150..156: hang jump up (q10)" $J 26:100:200 20:150:156
ccase $F hang_jump_back $S 300 $C "hang, Down 100..200 + Jump 150..156: free leap back (53.73 out + 34.77 up a frame)" $J 28:100:200 20:150:156
ccase $F hang_back_hold $S 260 $C "hang, Down held 100..200, no jump: back lean (S456) and release" $J 28:100:200
ccase $F hang_jump_right $S 300 $C "hang, Right 100..200 + Jump 112..118: side leap (70.80 aside + 34.77 up)" $J 27:100:200 20:112:118
ccase $F hang_jump_left $S 300 $C "hang, Left 100..200 + Jump 112..118: side leap" $J 25:100:200 20:112:118
ccase $F hang_drop $S 220 $C "hang, Crouch 100..106: let go, fall, land" $J 11:100:106
ccase $F hang_jump_neutral $S 260 $C "hang, Jump 100..106 with no stick" $J 20:100:106
F=jump
S2=1137.7,-3497,-2733,0
ccase $F stand_jump_open $S2 140 $C "standing jump ~637 short of ledge 973 (out of reach): plain jump and landing" 20:10:14
ccase $F stick_jump_open $S2 160 $C "Up + Jump at tick 10 (stick jump) towards ledge 973 from ~637 short" 26:10:60 20:10:14
ccase $F run_jump_bedroom "1137.7,-2600,-2733,0" 220 $C "run 10..70 then Jump at 40 towards the ledge 973 wall from ~1534 short" 26:10:70 20:40:46
