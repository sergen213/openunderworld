#!/bin/sh
# l4_temple balance beam 1079 (ChartB / ChartBlend): SPAWN at the beam as GameReal.TempleBalanceBeamWalkAndJump does
# (ours (-5500, -8099, 3100) facing +x; no checkpoint reaches the beam, so this take is subject to the post-SPAWN carry,
# README "Carry after SPAWN"), camera pinned behind her (+x). 2026-11-22 run 1: from that spawn she falls and catches
# the beam as a ledge (S447 hang, z 2569.5) and Up only cycles S434 / S446; so Jump 40..44 from the hang climbs onto
# the beam (bbeam_ledgeu_climbup, as GameReal.ChapelHangClimbsOntoBalanceBeam), then Up 160..460 (walk +x), Down
# 480..780 (turn, walk back). Per tick ANIM (playing pattern + queue), the main layer L +0xF4 / +0xF8 (raw: +0xF6
# sub-layer count, +0xF9), +0x8C / +0x90 / +0x94, weights *(L+0x84)[0..3], channel 0 *(L+0x80) +0x28 time / +0x50
# rate; instance p +0x28 / +0x358 / +0x35C / +0x3A0 / +0x3A4 / +0x3A8 / +0x3B4 (raw and float); player data +0xC94 /
# +0xC98 / +0xED0 / +0xCDC. Writes beam/t1079_walk_turn.tsv + case.json.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_beam.sh
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_temple; OFF=500.69,9432.76,-19909.81
r=0; until cunit l4_temple; do r=$((r+1)); [ $r -ge 3 ] && exit 1; done
export ANIM=1 VARS=lf4:L:F4,lf8:L:F8,l8c:L:8C:f,l90:L:90:f,l94:L:94:f,w0:w:0:f,w1:w:4:f,w2:w:8:f,w3:w:C:f,k28:k:28:f,k50:k:50:f,\
p28:p:28,p358:p:358,p35c:p:35C,p3a0:p:3A0,p3a4:p:3A4,p3a8:p:3A8,p3b4:p:3B4,p358f:p:358:f,p35cf:p:35C:f,p3a0f:p:3A0:f,\
p3a4f:p:3A4:f,p3a8f:p:3A8:f,p3b4f:p:3B4:f,c94:d:C94,c98:d:C98:f,ed0:d:ED0,ed0f:d:ED0:f,cdc:d:CDC,cdcf:d:CDC:f
HOLD=30 ccase beam t1079_walk_turn "-4999.31,1333.76,-16809.81,1.5708" 840 "1,0" \
    "SPAWN at beam 1079 as GameReal.TempleBalanceBeamWalkAndJump (ours -5500,-8099,3100 facing +x; no checkpoint reaches the beam: post-SPAWN carry possible, README); she catches the beam as a ledge, Jump 40..44 climbs onto it, Up 160..460 (walk +x), Down 480..780 (turn, walk back); camera pinned behind (+x)" \
    20:40:44 26:160:460 28:480:780
