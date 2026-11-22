#!/bin/sh
# Pole-to-pole flights with the flight columns (ChartJ: in-flight jump-target refine against Wine; no ground-to-pole take
# exists, chart_poleground.sh). Replays pole/tr_pole_fling (1336 -> 1337, ends S623) and pole/tr_pole_top_jump_fwd with
# ANIM=1 and pd +0x77C (flight time left, f), +0xA64 (jump-target markup), +0xA68 (param, f), +0x9F0..+0x9F8 (its
# point, f), +0xC20 / +0xC24 (f). Files: pole/<case>_flight.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_polefling.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7341.9,25802.0,-7298.2
[ "$1" = noload ] || cunit $UNIT || exit 1
export ANIM=1 VARS=pd_77c:pd:77C:f,pd_a64:pd:A64,pd_a68:pd:A68:f,pd_9f0:pd:9F0:f,pd_9f4:pd:9F4:f,pd_9f8:pd:9F8:f,pd_c20:pd:C20:f,pd_c24:pd:C24:f,pd_c94:pd:C94
for n in tr_pole_fling tr_pole_top_jump_fwd; do creplay "$OU_CHART_DIR/pole/$n.case.json" ${n}_flight; done
