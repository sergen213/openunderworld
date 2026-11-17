#!/bin/sh
# One Wine session in l2_cliffclimb (Coastal Thailand's sea and yacht), tru.exe frame (ours = tru + (-12446.2,
# -59357.3, 19003.8); the sea volume's static top is tru z -18895.82 = ours 108). Writes into $OUT:
#   repro-h300.txt   the existing trace work/waterentry-wine/h300.txt again (a plain drop from 300 over the top)
#   tread-yacht.txt  60 s treading 1000 units off the yacht's start position, tread-open.txt the same in open water
#   swim-*.txt       under-water swim start / stop, turn, ascend / descend, camera-pitch steering
#   breath.txt       a dive with no input and no god mode until the first drowning damage
#   deck-*.txt       the camera on the yacht's deck (idle, weapons drawn / holstered, run off into the sea)
#   camhook.txt      the camera mode requests (camhook.sh log) of the deck part
# Usage: OU_WREF_DIR=... WINEPREFIX=... OUT=dir sessions/l2_cliffclimb_sea.sh [parts...]  (parts default: all)
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"
: "${OUT:=$OU_WREF_DIR/out}"; mkdir -p "$OUT"
W="$WREF_SRC/w.sh"
R() { "$W" ref "$@"; }
PARTS=${*:-"repro tread swim breath deck"}
has() { case " $PARTS " in *" $1 "*) return 0;; esac; return 1; }
TOP=-18895.82
"$WREF_SRC/start-kwin.sh" || exit 1
"$WREF_SRC/newgame.sh" > "$OUT/newgame.log" 2>&1
"$W" wmem goto l2_cliffclimb; sleep 100
"$W" pole dump -1 > "$OUT/start.txt"; "$WREF_SRC/shot.sh" "$OUT/start.png"
tread() { # label x y : drop in 44 under the top, settle, then 60 s of treading with no input
    R drop "$2" "$3" "$(awk "BEGIN{print $TOP - 44}")" 500 3000 0 0 0 > "$OUT/tread-$1-drop.txt"
    GOD=1 R rec 60000 > "$OUT/tread-$1.txt"; "$WREF_SRC/shot.sh" "$OUT/tread-$1.png"
}
if has repro; then R drop -20777 44668 -18595.8 1500 4000 0 0 0 > "$OUT/repro-h300.txt"; fi
if has tread; then tread yacht -21150 44687; tread open -19500 45500; fi
if has swim; then
    R drop -19500 45500 "$(awk "BEGIN{print $TOP - 44}")" 500 3000 0 0 0 > /dev/null
    # dive (Ctrl tap), then Forward (Up) held 3 s and released
    GOD=1 R rec 10000 11:300:450 26:3000:6000 > "$OUT/swim-startstop.txt"
    # Forward held, Right held 3 s from 1.5 s, then Left 3 s
    GOD=1 R rec 10000 26:0:9000 27:1500:4500 25:5500:8500 > "$OUT/swim-turn.txt"
    # Forward held; Jump (Space) held 1..3 s, Crouch (Ctrl) held 4.5..6.5 s
    GOD=1 R rec 8000 26:0:7500 20:1000:3000 11:4500:6500 > "$OUT/swim-updown.txt"
    # Forward held; the mouse pitches the camera down at 1.5 s and back up at 4.5 s
    ( sleep 1.5; R mouse 0 400 10; sleep 2.8; R mouse 0 -400 10 ) &
    GOD=1 R rec 8000 26:0:7500 > "$OUT/swim-pitch.txt"; wait
    "$WREF_SRC/shot.sh" "$OUT/swim-end.png"
fi
if has breath; then
    R drop -19500 45500 "$(awk "BEGIN{print $TOP - 44}")" 500 3000 0 0 0 > /dev/null
    R rec 142000 11:300:450 > "$OUT/breath.txt"
    R god > /dev/null 2>&1 & GP=$!
    sleep 3; "$WREF_SRC/shot.sh" "$OUT/breath-end.png"
fi
if has deck; then
    pgrep -f "ref.exe god" > /dev/null || { R god > /dev/null 2>&1 & GP=$!; }
    "$WREF_SRC/camhook.sh" install > "$OUT/camhook-install.txt" 2>&1
    R tpv -22369.98 44705.42 -18792.41 1500 > /dev/null; "$W" pole yaw 2.3214; sleep 3
    R rec 15000 > "$OUT/deck-idle.txt"; "$WREF_SRC/shot.sh" "$OUT/deck-idle.png"
    R rec 7000 58:1000:1100 58:4500:4600 > "$OUT/deck-weapons.txt"
    R rec 10000 26:500:6000 > "$OUT/deck-to-sea.txt"; "$WREF_SRC/shot.sh" "$OUT/deck-to-sea.png"
    "$WREF_SRC/camhook.sh" log > "$OUT/camhook.txt" 2>&1
fi
[ -n "$GP" ] && kill "$GP" 2>/dev/null
"$WREF_SRC/stop.sh"
echo done > "$OUT/done.txt"
