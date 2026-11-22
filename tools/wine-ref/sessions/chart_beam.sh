#!/bin/sh
# l4_temple balance beam 1079 (ChartB / ChartBlend): SPAWN at the beam as GameReal.TempleBalanceBeamWalkAndJump does
# (ours (-5500, -8099, 3100) facing +x; no checkpoint reaches the beam, so this take is subject to the post-SPAWN carry,
# README "Carry after SPAWN": position columns suspect), camera pinned behind her (+x). From that spawn she falls and
# catches the beam as a ledge (S447 hang, z 2569.5); Jump 40..44 climbs onto the beam (bbeam_ledgeu_climbup).
#   (default) t1079_walk_turn: Up 160..460 (walk +x), Down 480..780 (turn, walk back) - 2026-11-22 run 2.
#   sweep: t1079_stick_sweep: Up 160..280, Left 300..420, Right 440..560, Down 580..700 (camera-relative), CAMDUMP.
# Per tick ANIM (playing pattern + queue), CAMDUMP (sweep), the stick record +0x8 / +0x18, the main layer L +0xF4 /
# +0xF8 (raw: +0xF6 sub-layer count, +0xF9 blend type, +0xF4 & 0x20 mirror), +0x8C / +0x90 / +0x94, weights
# *(L+0x84)[0..5], channel 0 *(L+0x80) +0x28 time / +0x50 rate; the chart agent A = *(playerData+0xF10) (ChartB: the
# beam controller 0x743900 runs on A+0x358): +0x28 heading target, +0x358 beam heading, +0x35C beam angle, +0x3A0 lean,
# +0x3A4 walk amount, +0x3A8 target - beam angle, +0x3B4 held stick heading; player data +0xC94 / +0xC98 / +0xED0.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_beam.sh [sweep]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_temple; OFF=500.69,9432.76,-19909.81
r=0; until cunit l4_temple; do r=$((r+1)); [ $r -ge 3 ] && exit 1; done
export ANIM=1 VARS=lf4:L:F4,lf8:L:F8,l8c:L:8C:f,l90:L:90:f,l94:L:94:f,w0:w:0:f,w1:w:4:f,w2:w:8:f,w3:w:C:f,w4:w:10:f,w5:w:14:f,\
k28:k:28:f,k50:k:50:f,s08:s:8:f,s18:s:18:f,a28:c:28:f,a358:c:358:f,a35c:c:35C:f,a3a0:c:3A0:f,a3a4:c:3A4:f,a3a8:c:3A8:f,\
a3b4:c:3B4:f,c94:d:C94,c98:d:C98:f,ed0:d:ED0,cc0:d:CC0:f,cc4:d:CC4:f,cc8:d:CC8:f,s1b0:S:1B0:f,s1b4:S:1B4:f,s1b8:S:1B8:f,s1c0:S:1C0
if [ "$1" = sweep ]; then
    CAMDUMP=1 HOLD=30 ccase beam t1079_stick_sweep "$S" 760 "1,0" \
        "SPAWN at beam 1079 (ours -5500,-8099,3100 facing +x; no checkpoint reaches the beam: post-SPAWN carry, position columns suspect, README); she catches the beam as a ledge, Jump 40..44 climbs onto it; stick sweep camera-relative Up 160..280, Left 300..420, Right 440..560, Down 580..700; camera pinned behind (+x); CAMDUMP" \
        20:40:44 26:160:280 25:300:420 27:440:560 28:580:700
else
    HOLD=30 ccase beam t1079_walk_turn "$S" 840 "1,0" \
        "SPAWN at beam 1079 as GameReal.TempleBalanceBeamWalkAndJump (ours -5500,-8099,3100 facing +x; no checkpoint reaches the beam: post-SPAWN carry possible, README); she catches the beam as a ledge, Jump 40..44 climbs onto it, Up 160..460 (walk +x), Down 480..780 (turn, walk back); camera pinned behind (+x)" \
        20:40:44 26:160:460 28:480:780
fi
