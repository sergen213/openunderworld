#!/bin/sh
# l4_traprooms offset check (Main): cmeasure (instance agreement) and tru.exe's markup of poles 1336 / 1337, read like
# ledge 1063 (chart_markupdump.sh): *(0xFADB58)+0xC94 is a list node, node+0x20 the MarkUp (+0x0C owner, +0x2E id,
# +0x34 centre, +0x58 -> polyline {n; pad; float4 points[n]} at +0x10). 1336 after replaying pole/tr_pole_grab, 1337
# after pole/tr_pole_fling (ends on 1337). Also the floor under GameReal's spot: a 600-unit drop from tru
# (6958.1, 4474, -298.2) with its z per tick. Writes $OU_WREF_DIR/traprooms_check.txt.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_traprooms_offset.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7341.9,25802.0,-7298.2
[ "$1" = noload ] || cunit $UNIT || exit 1
W="$WREF_SRC/w.sh"; O="$OU_WREF_DIR/traprooms_check.txt"
rdw() { "$W" wmem rd "$1" "$2" | tr -d '\r'; }
le32() { echo "$1" | tr -s ' \n' ' ' | awk -v o="$2" '{i=o+1; printf "%s%s%s%s", $(i+3), $(i+2), $(i+1), $i}'; }
dumpmk() { # LABEL: the attached markup's box and polyline as hex
    PD=$(le32 "$(rdw FADB58 4)" 0); ND=$(le32 "$(rdw "$(printf %x $((0x$PD + 0xC94)))" 4)" 0)
    MK=$(le32 "$(rdw "$(printf %x $((0x$ND + 0x20)))" 4)" 0)
    echo "$1 node $ND markup $MK"; rdw "$MK" 96 | sed "s/^/$1 box /"
    PL=$(le32 "$(rdw "$MK" 96)" $((0x58))); echo "$1 polyline $PL"; rdw "$PL" 64 | sed "s/^/$1 poly /"; }
{ cmeasure $UNIT 2>&1
  creplay "$OU_CHART_DIR/pole/tr_pole_grab.case.json" tr_pole_grab_offchk > /dev/null; dumpmk p1336
  creplay "$OU_CHART_DIR/pole/tr_pole_fling.case.json" tr_pole_fling_offchk > /dev/null; dumpmk p1337
  SPAWN=6958.1,4474,-298.2,3.14159 HOLD=2 "$W" chart trace 120 | tr -d '\r' | awk -F'\t' 'NR>1 && NR%10==2{print "floorprobe", $1, $2, $6, $7, $8}'
} | tee "$O"
rm -f "$OU_CHART_DIR"/pole/tr_pole_*_offchk.*
