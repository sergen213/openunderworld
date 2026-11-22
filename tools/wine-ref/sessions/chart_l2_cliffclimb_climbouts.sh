#!/bin/sh
# Chart reference corpus, l2_cliffclimb swim climb-outs round ledge 936 (all eleven points z 158 ours; the only ledge near
# the sea): treading 180 out from the middle of each segment at the surface (tru z -18939.8), facing it, camera along
# that direction, Forward from tick 60 (spawns: tru frame, offset (12446.22, 59357.28, -19003.77) measured). Looking
# for S242 (`swim_climbout`, request 891: from S237 T1115 on PSCCF_ClimbOutHeightOK 0x32F) and S1253
# (`swim_climbout_to_wallhug` 887: S237 T1110 on clearance result 657, a shelf: seg 9 is one in our survey).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l2_cliffclimb_climbouts.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l2_cliffclimb; OFF=12446.22,59357.28,-19003.77
[ "$1" = noload ] || cunit $UNIT || exit 1
HOLD=${HOLD:-60}; RESETKEYS=""; F=swim
for s in "1 -12025.2,55550.3,-18939.8,1.7019 0.991,0.131" "2 -11779.7,55188.0,-18939.8,2.5487 0.559,0.829" \
         "3 -11379.2,55043.4,-18939.8,3.0586 0.083,0.997" "4 -10999.9,55055.7,-18939.8,3.2840 -0.142,0.990" \
         "5 -10633.8,55198.7,-18939.8,3.7933 -0.607,0.795" "6 -10427.2,55485.9,-18939.8,4.4152 -0.956,0.293" \
         "7 -10357.1,55823.1,-18939.8,4.5646 -0.989,0.147" "8 -10382.8,56262.4,-18939.8,-1.2780 -0.957,-0.289" \
         "9 -10520.3,56595.5,-18939.8,-1.0707 -0.878,-0.480"; do
    set -- $s
    ccase $F cc936_seg$1_climbout $2 360 $3 "treading 180 out from the middle of ledge 936 seg $1, Forward held 60..300: climb out" 26:60:300
done
ccase $F cc936_seg9_climbout_walk "-10520.3,56595.5,-18939.8,-1.0707" 360 -0.878,-0.480 "seg 9, Walk + Forward held 60..300 (slow approach)" a4:60:300 26:60:300
ccase $F cc936_seg9_climbout_dive "-10520.3,56595.5,-18939.8,-1.0707" 400 -0.878,-0.480 "seg 9, Crouch 60..64 (dive), Forward held 100..340: from under water" 11:60:64 26:100:340
