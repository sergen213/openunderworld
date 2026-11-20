#!/bin/sh
# OU-CMB-003: the l2_cliffclimb sharks around a treading / diving Lara, tru.exe frame (tru = ours + (12446.2, 59357.3,
# -19003.8); the sea volume's static top is tru z -18895.82 = ours 108). Writes into $OUT:
#   enemies.txt          enemy instances after the unit load (enemy list)
#   zones-<p>.txt        the sharks' guard zones and region (enemy zones) with Lara's position, after each placement
#   tread-<p>.txt        300 s treading with no input at the yacht / open-water points of l2_cliffclimb_sea.sh:
#                        Lara, health (bites as "hurt"), every shark's position, distance, Lara-sense flags and
#                        behaviour objects
#   dive.txt             a dive (Ctrl tap) at the open-water point, then 120 s lying still under water
#   depth-<d>.txt        Lara held (tpv) <d> units under the volume top at the open-water point for $DEPTH_MS, the
#                        sharks watched meanwhile (zones-depth-<d>.txt at the start); 4 s at the top in between
# Usage: OU_WREF_DIR=... OU_WREF_NAME=... OUT=dir sessions/l2_cliffclimb_sharks.sh [parts...]
# parts: yacht open dive depth (default: yacht open dive)
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
: "${OUT:=$OU_WREF_DIR/out}"; mkdir -p "$OUT"
W="$WREF_SRC/w.sh"
PARTS=${*:-"yacht open dive"}
has() { case " $PARTS " in *" $1 "*) return 0;; esac; return 1; }
TOP=-18895.82
"$WREF_SRC/start-kwin.sh" || exit 1
"$WREF_SRC/newgame.sh" > "$OUT/newgame.log" 2>&1
"$W" wmem goto l2_cliffclimb; sleep 100
"$W" pole dump -1 > "$OUT/start.txt"; "$WREF_SRC/shot.sh" "$OUT/start.png"
"$W" enemy list > "$OUT/enemies.txt"
UIDS=$(awk '$NF ~ /shark/ {print $1}' "$OUT/enemies.txt" | tr '\n' ' ')
echo "sharks: $UIDS" >> "$OUT/enemies.txt"
drop() { "$W" ref drop "$2" "$3" "$(awk "BEGIN{print $TOP - 44}")" 500 3000 0 0 0 > /dev/null; "$W" enemy zones $UIDS > "$OUT/zones-$1.txt"; }
: "${TREAD_MS:=300000}"
if has yacht; then drop yacht -21150 44687; "$W" enemy watch "$TREAD_MS" 100 $UIDS > "$OUT/tread-yacht.txt"; "$WREF_SRC/shot.sh" "$OUT/tread-yacht.png"; fi
if has open; then drop open -19500 45500; "$W" enemy watch "$TREAD_MS" 100 $UIDS > "$OUT/tread-open.txt"; "$WREF_SRC/shot.sh" "$OUT/tread-open.png"; fi
if has dive; then
    drop dive -19500 45500
    ( sleep 0.3; "$W" wkey key 11 150 > /dev/null ) &
    "$W" enemy watch 120000 100 $UIDS > "$OUT/dive.txt"; wait
    "$WREF_SRC/shot.sh" "$OUT/dive.png"
fi
if has depth; then # pinned at each depth; the watch runs beside the pin
    : "${DEPTH_MS:=40000}" "${DEPTHS:=100 250 400 700}"
    for d in $DEPTHS; do
        z=$(awk "BEGIN{print $TOP - $d}")
        "$W" ref tpv -19500 45500 "$(awk "BEGIN{print $TOP - 44}")" 4000 > /dev/null
        ( "$W" ref tpv -19500 45500 "$z" "$DEPTH_MS" > /dev/null ) &
        sleep 1; "$W" enemy zones $UIDS > "$OUT/zones-depth-$d.txt"
        "$W" enemy watch "$((DEPTH_MS - 1500))" 100 $UIDS > "$OUT/depth-$d.txt"; wait
    done
fi
"$WREF_SRC/stop.sh"
echo done > "$OUT/done.txt"
