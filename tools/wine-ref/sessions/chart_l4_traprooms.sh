#!/bin/sh
# Chart reference corpus, l4_traprooms (goto unit: tru = ours + (-7341.9, 25802.0, -7298.2), REFERENCE_COMPARISON M67).
# Poles 1336 / 1337 (GameReal.TrapRoomsPoleFling): markup 4, axis z 7133 ours, 1264 apart in y; a standing jump from
# ours (14300, -21328, 6400) facing +y grabs 1336 in ours; in tru.exe the same spot's standing jump does not reach it
# (2026-11-21: plain jumps), so the cases pin her 250 over 1336's idle hang root (tru (6942.22, 4947.40, -165.58),
# polefid2/fling1.txt) facing +y (tru yaw pi, camera (0,1)): the fall's air grab takes the pole (M07).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l4_traprooms.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7341.9,25802.0,-7298.2
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}
F=pole
P=6942.22,4947.40,84.42,3.14159; CP=0,1; G=""
ccase $F tr_pole_grab $P 200 $CP "pinned 250 over pole 1336's idle hang root: falls, air grab, hand-over to the idle hang"
ccase $F tr_pole_swing $P 400 $CP "grab, Up held 120..380: swing" 26:120:380
ccase $F tr_pole_fling $P 420 $CP "grab, Up held 120..300 + Jump 220..224: fling to pole 1337 (leaves 624..628 ms after the press, M07)" 26:120:300 20:220:224
ccase $F tr_pole_drop $P 260 $CP "grab, Crouch 120..124: drop" 11:120:124
ccase $F tr_pole_top $P 360 $CP "grab, Jump 140..144 with no stick: onto the pole top (hpole_to_perch, root 19.9 over the axis)" 20:140:144
ccase $F tr_pole_top_jump_up $P 460 $CP "pole top, then Jump 300..304: q35 straight up, back on top" 20:140:144 20:300:304
ccase $F tr_pole_top_jump_fwd $P 460 $CP "pole top, then Up 300..340 + Jump 304..308: jump forward (q33 / next pole, M08)" 20:140:144 26:300:340 20:304:308
ccase $F tr_pole_top_drop $P 400 $CP "pole top, then Crouch 300..304: back down onto the hang" 20:140:144 11:300:304
ccase $F tr_pole_hoh_right $P 400 $CP "grab, Right held 120..380: turn along the pole / hand over hand" 27:120:380
ccase $F tr_pole_hoh_left $P 400 $CP "grab, Left held 120..380" 25:120:380
