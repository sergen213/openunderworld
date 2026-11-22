#!/bin/sh
# Chart reference corpus, l4_temple perch 1190 on pillar 1191 (GameReal.TemplePerchTurnsAndPillarTop: her root on the top
# at ours (-6620, -12345, -1700)): a fresh session, these cases first (a pillar / perch hold survives the teleport, so
# nothing may hold her before them). She starts 200 south of 1191 facing it (+y, camera (0,1)), walks into it and climbs
# to the top (Up held), where pillar_detach_perch puts her on the perch (as on 1194, 2026-11-21); then the perch keys.
# One climb per case: each case after the first starts on the perch or pillar the previous one left (start.state).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l4_temple_perch.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_temple
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}; GSETTLE=150; RESET=0; F=perch; CY=0,1
P=$(tru -6620 -12545 -1500 90)
cground $F p1190_climb_to_perch $P 1500 $CY "200 south of pillar 1191, Up held 100..1400: walk in, climb, pillar_detach_perch onto perch 1190" 26:100:1400
SETTLE=0; HOLD=0
ccase $F p1190_turn_left $P 200 $CY "on perch 1190 (as the previous case left her; no placement), Left 20..50: turn" 25:20:50
ccase $F p1190_turn_right $P 200 $CY "on the perch, Right 20..50: turn" 27:20:50
ccase $F p1190_turn_back $P 260 $CY "on the perch, Down 20..50: turn round / down" 28:20:50
ccase $F p1190_jump_fwd $P 300 $CY "on the perch, Up 20..60 + Jump 24..28: jump to the next perch / ledge" 26:20:60 20:24:28
