#!/bin/sh
# Status 2026-11-21: the start-minus-intro offset (34604.6, 3795.9, 12724.6) put her in a fall, not at ladder 234
# (no ladder states reached; traces discarded): the unit's start is not its 'player' intro. Measure the offset from an
# instance before re-running.
# Chart reference corpus, l1_ship_sinking_hall ladder 234 (GameReal.ShipSinkingHallLadder): in the valve wall's plane
# (y 29 ours, open side -y), 158 wide from x 1401; she comes onto it out of the air in front of it (the shaft has no
# floor) from ours (1480, -120, 1100) facing +y with the stick forward. The offset (tru = ours + d) is measured here
# after the goto: d = her start position - our 'player' intro (-978.4, -10539.4, 29.2) (ChartCore, ou-game log).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l1_ship_sinking_hall.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l1_ship_sinking_hall
[ "$1" = noload ] || cunit $UNIT || exit 1
OFF=${OFF:-$("$WREF_SRC/w.sh" wmem state | tr -d '\r' | awk '{for(i=1;i<=NF;i++) if($i=="pos") printf "%.2f,%.2f,%.2f", $(i+1)+978.4, $(i+2)+10539.4, $(i+3)-29.2}')}
echo "offset $OFF"
HOLD=${HOLD:-60}
L=$(echo $OFF | awk -F, '{printf "%.2f,%.2f,%.2f,3.14159", 1480+$1, -120+$2, 1100+$3}'); CL=0,1
F=ladder
ccase $F sh_ladder_attach $L 260 $CL "in the air in front of ladder 234, Up held 0..40: attach out of the air (ladder_attach_M), settle on a rung" 26:0:40
ccase $F sh_ladder_climb_up $L 460 $CL "attach, Up held 140..420: climb (rungs 110.619 apart, M96)" 26:0:40 26:140:420
ccase $F sh_ladder_climb_down $L 460 $CL "attach, Down held 140..420: climb down" 26:0:40 28:140:420
ccase $F sh_ladder_jump_up $L 360 $CL "attach, Up 140..200 + Jump 150..154: jump up (q23 80.15 a frame)" 26:0:40 26:140:200 20:150:154
ccase $F sh_ladder_jump_back $L 360 $CL "attach, Jump 150..154 with no stick: leap back (q22 47.41 + 60.05)" 26:0:40 20:150:154
ccase $F sh_ladder_left $L 360 $CL "attach, Left held 140..330: side reach / jump antic (OU-PLY-004)" 26:0:40 25:140:330
ccase $F sh_ladder_right_jump $L 360 $CL "attach, Right 140..220 + Jump 180..184: side jump (OU-PLY-004)" 26:0:40 27:140:220 20:180:184
ccase $F sh_ladder_drop $L 300 $CL "attach, Crouch 150..154: let go" 26:0:40 11:150:154
