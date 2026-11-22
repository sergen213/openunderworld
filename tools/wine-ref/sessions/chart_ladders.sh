#!/bin/sh
# Chart reference corpus, ladders in l1_ship_sinking_hall (collision markup type 9, tools/wine-ref/ours/markup.cpp):
#   234: plane y 29.4 ours, x 1401.3..1559.3, z 940.2..1730.4;  236: the same plane, z 2196.2..3786.3 (above 234);
#   235: plane y -1247.3, x -4899.0..-4740.9, z 6022.2..7500.7.  (Rungs 110.619 apart, M96.)
# The offset is measured (cmeasure). The 2026-11-21 run from GameReal.ShipSinkingHallLadder's spawn (ours (1480, -120,
# 1100), 150 out) fell past 234 to a floor 30 ticks below (~ours z 460); these placements try 110.6 out (the rung
# standoff), from the floor under 234 (a jump to its bottom rung), 236 from the air, and both sides of 235. Each
# placement first gets an attach probe (`*_attach`); the action cases follow for every placement (keys after tick 120).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_ladders.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l1_ship_sinking_hall
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}; F=ladder; RESETKEYS="11:5:9"
act() { # $1 tag $2 spawn $3 cam $4.. attach keys
    t=$1; sp=$2; c=$3; shift 3
    ccase $F lad_${t}_attach $sp 260 $c "ladder $t: attach probe" "$@"
    ccase $F lad_${t}_climb_up $sp 560 $c "ladder $t: attach, Up held 140..520: climb up" "$@" 26:140:520
    ccase $F lad_${t}_climb_down $sp 560 $c "ladder $t: attach, Down held 140..520: climb down" "$@" 28:140:520
    ccase $F lad_${t}_top_exit $sp 1300 $c "ladder $t: attach, Up held 140..1250: to the top and off it" "$@" 26:140:1250
    ccase $F lad_${t}_bottom $sp 1100 $c "ladder $t: attach, Down held 140..1050: to the bottom (exit / lean back)" "$@" 28:140:1050
    ccase $F lad_${t}_fast_up $sp 560 $c "ladder $t: attach, Up 140..520 + Interact held 160..520: fast climb" "$@" 26:140:520 45:160:520
    ccase $F lad_${t}_fast_down $sp 560 $c "ladder $t: attach, Down 140..520 + Interact held 160..520: fast slide down" "$@" 28:140:520 45:160:520
    ccase $F lad_${t}_left_hold $sp 420 $c "ladder $t: attach, Left held 140..380: side reach / antic" "$@" 25:140:380
    ccase $F lad_${t}_right_hold $sp 420 $c "ladder $t: attach, Right held 140..380: side reach / antic" "$@" 27:140:380
    ccase $F lad_${t}_left_jump $sp 420 $c "ladder $t: attach, Left 140..240 + Jump 190..194: side jump" "$@" 25:140:240 20:190:194
    ccase $F lad_${t}_right_jump $sp 420 $c "ladder $t: attach, Right 140..240 + Jump 190..194: side jump" "$@" 27:140:240 20:190:194
    ccase $F lad_${t}_jump_up $sp 420 $c "ladder $t: attach, Up 140..220 + Jump 170..174: jump up (q23)" "$@" 26:140:220 20:170:174
    ccase $F lad_${t}_jump_back $sp 420 $c "ladder $t: attach, Jump 170..174, no stick: leap back (q22)" "$@" 20:170:174
    ccase $F lad_${t}_drop $sp 360 $c "ladder $t: attach, Crouch 170..174: let go" "$@" 11:170:174
}
act a234 $(tru 1480 -81.2 1300 90) 0,1 26:0:60
G=$GSETTLE; GSETTLE=120; SETTLE=120
act f234 $(tru 1480 -150 700 90) 0,1 26:100:140 20:104:108
SETTLE=0; GSETTLE=$G
ccase $F lad_a236_attach $(tru 1480 -81.2 2600 90) 260 0,1 "ladder 236 from the air 110.6 out, Up 0..60: attach probe" 26:0:60
ccase $F lad_n235_attach $(tru -4820 -1357.9 6400 90) 260 0,1 "ladder 235 from -y, 110.6 out, Up 0..60: attach probe" 26:0:60
ccase $F lad_s235_attach $(tru -4820 -1136.7 6400 -90) 260 0,-1 "ladder 235 from +y, 110.6 out, Up 0..60: attach probe" 26:0:60
