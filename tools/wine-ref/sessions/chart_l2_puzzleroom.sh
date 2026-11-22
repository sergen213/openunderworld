#!/bin/sh
# Chart reference corpus, wall hug: l2_puzzleroom shelf 476 (ledge from ours (-14321.7, 43518.4) to (-15489.3, 43518.4),
# z 1985.1; GameReal.PuzzleRoomWallHug): she starts 100 along it from its first end, 88.5 in from the line on the shelf
# side (-y), at z +20, facing along it (-x, yaw 180 ours); camera along (-1,0) so Up walks along the shelf, Down back.
# The offset is measured (cmeasure). Entry by walking (bbeam_walk_turn180 -> Wall_Hug_Idle), sidesteps both ways,
# walk off at either end.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l2_puzzleroom.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l2_puzzleroom
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}; F=wallhug; RESETKEYS="28:5:40"
S=$(tru -14421.7 43429.9 2005.1 180); C=-1,0
cground $F ph476_enter $S 300 $C "Up held 10..200 along shelf 476: walk onto it, turn to the wall (wall hug entry)" 26:10:200
cground $F ph476_shimmy_along $S 500 $C "Up held 10..460: entry, then the sidestep along the shelf" 26:10:460
cground $F ph476_enter_back $S 600 $C "Up 10..200 (entry), then Down held 260..560: sidestep back to the first end, walk off" 26:10:200 28:260:560
cground $F ph476_walk_off_far $S 2400 $C "Up held 10..2350: along the whole shelf (1168 long) and off its far end" 26:10:2350
cground $F ph476_hug_jump $S 400 $C "Up 10..200 (entry), Jump 260..264: jump from the wall hug (q68 / q55)" 26:10:200 20:260:264
cground $F ph476_hug_side_jump $S 400 $C "Up 10..240 (entry, sidestep), Jump 220..224 with the stick held: side jump" 26:10:240 20:220:224
cground $F ph476_hug_crouch $S 400 $C "Up 10..200 (entry), Crouch 260..264: drop off the shelf" 26:10:200 11:260:264
# Walking variants (the run above left the shelf at its edge into the ledge attach, S11 -> S543, no wall hug):
cground $F ph476_walk_enter $S 600 $C "Walk (Alt) + Up held 10..560 along shelf 476: slow approach to the wall-hug entry" a4:10:560 26:10:560
cground $F ph476_walk_enter_back $S 900 $C "Walk + Up 10..400, then Walk + Down 460..860: along and back" a4:10:400 26:10:400 a4:460:860 28:460:860
