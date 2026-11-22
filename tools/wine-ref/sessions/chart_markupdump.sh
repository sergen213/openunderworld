#!/bin/sh
# The markup tru.exe's l3_chapel `ledge/ch_hang_*` catches attach to (ChartB): replay ch_hang_land, then read the
# attached markup pointer *(0xFADB58)+0xC94 and dump the MarkUp box (0x80 bytes: +0x0C owner instance, +0x2C introID,
# +0x2E id, +0x30 segment, +0x33 type, +0x34 position, +0x40 box, +0x58 -> polyline {int n; pad; float4 points[n]};
# docs/formats/COLLISION.md) and its polyline, plus the owner's uid / position / object name when it has one.
# Result 2026-11-21: *(pd+0xC94) is not the MarkUp box itself but a 0x40-byte list node (+0x00 / +0x04 / +0x08 links,
# +0x20 -> the MarkUp, +0x24 -> a second record carrying the same id at +0x48); the MarkUp at node+0x20: +0x0C owner 0
# (static unit markup), +0x2E id 1063, +0x34 centre, +0x40 / +0x4C box half extents, +0x58 -> polyline (n at +0x08 of
# the polyline block here: 2 points). Ledge 1063 = our static ledgegrab 1063. The script dumps the node; read node+0x20.
# Writes $OU_WREF_DIR/markup_chapel.txt. Usage (session dir exported): sessions/chart_markupdump.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_chapel; OFF=3196.89,-2412.27,3634.58
[ "$1" = noload ] || cunit $UNIT || exit 1
W="$WREF_SRC/w.sh"
OU_CHART_DIR=$OU_WREF_DIR/t VARS=pd_c94:pd:C94 creplay "$OU_WORK/chart/wine/ledge/ch_hang_land.case.json" ch_hang_land_dump ledge
rdw() { "$W" wmem rd "$1" "$2" | tr -d '\r'; }
le32() { echo "$1" | tr -s ' \n' ' ' | awk -v o="$2" '{i=o+1; printf "%s%s%s%s", $(i+3), $(i+2), $(i+1), $i}'; }
PD=$(le32 "$(rdw FADB58 4)" 0); MK=$(le32 "$(rdw $(printf %x $((0x$PD + 0xC94))) 4)" 0)
O="$OU_WREF_DIR/markup_chapel.txt"
{ echo "pd $PD markup $MK"; rdw "$MK" 128 | sed 's/^/box /'
  PL=$(le32 "$(rdw "$MK" 128)" $((0x58))); OW=$(le32 "$(rdw "$MK" 128)" $((0x0C)))
  echo "polyline $PL owner $OW"; [ "$PL" != 00000000 ] && rdw "$PL" 272 | sed 's/^/poly /'
  [ "$OW" != 00000000 ] && "$W" wmem inst | tr -d '\r' | grep -i "^$OW " | sed 's/^/owner /'
} > "$O"
cat "$O"
