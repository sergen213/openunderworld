#!/bin/sh
# Chart reference corpus, wall-hug corners: l7_arctic_sea shelf 2054 (ours (16908.7, -45081.9) -> (17604.0, -45081.9) ->
# (17604.0, -46251.3), z -19688.9; GameReal.WallHugRoundsConcaveAndConvexCorners): the shelf side of seg 0 is +y, of
# seg 1 +x, so the joint is an outside corner. She starts 100 along seg 0 from its first end, 88.5 in, facing +x (yaw 0
# ours), camera (1,0): Up walks onto the shelf (entry) and on along it to the corner and round it.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l7_arctic_sea.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l7_arctic_sea
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}; F=wallhug; RESETKEYS="28:5:40"
S=$(tru 17008.7 -44993.4 -19668.9 0); C=1,0
cground $F as2054_enter $S 300 $C "Up held 10..200 along shelf 2054 seg 0: entry" 26:10:200
cground $F as2054_corner $S 1800 $C "Up held 10..1750: entry, sidestep 595 to the outside corner and round it onto seg 1" 26:10:1750
cground $F as2054_corner_back $S 2400 $C "Up 10..1500 (to and round the corner), then Down held 1560..2350: back round it" 26:10:1500 28:1560:2350
