#!/bin/sh
# Chart reference corpus, l2_junglehub (goto unit: tru = ours + (8558.2, -19414.5, -18073.1), docs/REFERENCE_COMPARISON.md).
# Streambed columns (gameplaygoal/reference/chimney3.sh): column A's east face x -12659 ours, B's west face ~-12021,
# ledge 796 on A (hang root ours (-12493, 23254, 5855.2)), ledge 797 above; facing -x (tru yaw -pi/2, camera (-1,0)).
# Ledge 2 (GameReal.JunglehubVaultOver): 273-high, 240-thick wall run into from ours (-9422.8, 18174.8, 4100) facing -y.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l2_junglehub.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l2_junglehub; OFF=8558.2,-19414.5,-18073.1
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}
FL=-3870.8,3839.5,-13642.1,-1.5708; CA=-1,0
train() { S=""; q=$1; while [ $q -lt $2 ]; do S="$S 20:$q:$((q+4))"; q=$((q+9)); done; echo $S; }
F=chimney
cground $F jh_stand_jump_wall $FL 160 $CA "floor 230 east of column A's east face, facing it: standing jump (q2, no cling, M06)" 20:20:24
cground $F jh_stick_jump_wall $FL 200 $CA "Up 20..56 + Jump 23..27: stick jump into column A: wall cling, hold 12 frames, detach fall (M06)" 26:20:56 20:23:27
cground $F jh_stick_jump_rebound $FL 260 $CA "stick jump into A, then a Jump every 9 ticks 56..200: rebounds between the columns" 26:20:56 20:23:27 $(train 56 200)
HG=-3928.8,3839.5,-11617.9,-1.5708
ccase $F jh_hang796_leap_back $HG 400 $CA "pinned 600 over ledge 796's hang root, falls and catches; Down 120..160 + Jump 124..128: leap back to column B, then no press: hold, detach fall" 28:120:160 20:124:128
ccase $F jh_hang796_climb $HG 520 $CA "catch 796, Down + Jump at 124, then a Jump every 9 ticks 150..450: B -> A -> B -> ledge 797 (M06 the climb)" 28:120:160 20:124:128 $(train 150 450)
F=hurdle
VA=-864.6,-1239.7,-13973.1,0
cground $F jh_ledge2_run_vault $VA 200 0,-1 "run (Up 20..160) into ledge 2's 273-high wall: hurdle_low_over (class 5 / 0x73DFF0 class 0)" 26:20:160
cground $F jh_ledge2_walk $VA 360 0,-1 "walk (Alt + Up 20..340) into ledge 2's wall" a4:20:340 26:20:340
cground $F jh_ledge2_jump $VA 200 0,-1 "standing jump at 20 in front of ledge 2's wall" 20:20:24
