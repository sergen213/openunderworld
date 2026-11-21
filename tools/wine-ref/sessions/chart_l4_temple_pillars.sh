#!/bin/sh
# Chart reference corpus, l4_temple pillars and perch: pillar 1194 (axis ours (-3160.5, -10745.7), floor ours z -2528.4)
# and pillar 1191 with perch 1190 on its top (GameReal.TemplePerchTurnsAndPillarTop: her root on the top at ours
# (-6620, -12345, -1700)). The offset is measured (cmeasure) against the doc's (500.70, 9432.76, -19909.81). Every take
# has its own file (`_t<N>`, TAKES, default "1 2"): an earlier re-run overwrote the first pillar traces. Spawns 200
# south of an axis facing it (+y, yaw 90 ours, camera (0,1)); SETTLE 150 lets the pin's fall end in the idle.
#   pillar: walk in (Forward from tick 100: pillar_attach_ground), or a jump at it (pillar_attach_M from the air).
#   perch: Forward held into pillar 1191 and up it: at the top pillar_detach_perch puts her on the perch; then turns,
#   down, jump, drop.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l4_temple_pillars.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_temple; OFF=500.70,9432.76,-19909.81
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || echo "offset not measured: the doc's kept" >&2
HOLD=${HOLD:-60}; GSETTLE=150; CY=0,1
RESET=300; RESETKEYS="28:5:240 11:245:249"   # a pillar / perch hold survives the teleport: slide down, let go
P4=$(tru -3160.5 -10945.7 -2400 90); P1=$(tru -6620 -12545 -1500 90)
for t in ${TAKES:-1 2}; do
F=pillar
cground $F t1194_walk_into_t$t $P4 300 $CY "Up held 100..260: walks into pillar 1194, attaches (pillar_attach_ground)" 26:100:260
cground $F t1194_climb_up_t$t $P4 460 $CY "Up held 100..420: walk in, attach, climb (312 units/s)" 26:100:420
cground $F t1194_slide_down_t$t $P4 560 $CY "Up 100..300 (attach, climb), then Down 360..500: slide down" 26:100:300 28:360:500
cground $F t1194_round_left_t$t $P4 520 $CY "Up 100..180 (attach), then Left held 240..480: round the shaft" 26:100:180 25:240:480
cground $F t1194_round_right_t$t $P4 520 $CY "Up 100..180 (attach), then Right held 240..480" 26:100:180 27:240:480
cground $F t1194_jump_back_t$t $P4 520 $CY "Up 100..300 (attach, climb), then Down 340..400 + Jump 350..354: jump off backwards (q48)" 26:100:300 28:340:400 20:350:354
cground $F t1194_jump_side_t$t $P4 520 $CY "Up 100..300, then Left 340..400 + Jump 350..354: jump off sideways" 26:100:300 25:340:400 20:350:354
cground $F t1194_jump_at_t$t $P4 300 $CY "Up 100..140 + Jump 104..108: running / stick jump at the pillar (pillar_attach_M)" 26:100:140 20:104:108
F=perch
cground $F t1191_climb_to_perch_t$t $P1 1500 $CY "Up held 100..1400: walk into pillar 1191, climb to its top, pillar_detach_perch onto perch 1190" 26:100:1400
cground $F t1191_perch_turn_left_t$t $P1 1700 $CY "up onto perch 1190 (Up 100..1400), then Left 1460..1490: turn on the spot" 26:100:1400 25:1460:1490
cground $F t1191_perch_turn_right_t$t $P1 1700 $CY "up onto the perch, then Right 1460..1490" 26:100:1400 27:1460:1490
cground $F t1191_perch_down_t$t $P1 1800 $CY "up onto the perch, then Down 1460..1490: back down onto the pillar (pillar_attach_perch)" 26:100:1400 28:1460:1490
cground $F t1191_perch_jump_t$t $P1 1800 $CY "up onto the perch, then Jump 1460..1464: jump off the perch" 26:100:1400 20:1460:1464
cground $F t1191_perch_jump_fwd_t$t $P1 1800 $CY "up onto the perch, then Up 1460..1500 + Jump 1464..1468: jump forwards (to the next perch / ledge)" 26:100:1400 26:1460:1500 20:1464:1468
cground $F t1191_perch_crouch_t$t $P1 1800 $CY "up onto the perch, then Crouch 1460..1464: drop" 26:100:1400 11:1460:1464
done
