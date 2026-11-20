#!/bin/sh
# One Wine session: Lara born on a chosen checkpoint at a fresh load of its unit (BUGS OU-SAV-002, docs/GAMEPLAY.md
# "Checkpoints and respawn"). 0x5D40C0 births her on the intro whose id is in the player's level data (*0xC59264 +0x90,
# or +0x114 when the short at +0x1F4 is set); with the id written there before `wmem goto` (and kept there while the
# unit loads), the load births her on that checkpoint. `ref rec` runs from the goto on (it re-reads the player every
# sample), so OUT/<tag>-rec.txt covers the birth and the first REC_MS ms; NOGOD=1 leaves her health alone (bites).
# Usage: OU_WREF_DIR=... WINEPREFIX=... OUT=dir sessions/checkpoint_respawn.sh <unit> <checkpoint uid> <tag>
# Outputs <tag>-ld.txt (level data before / after), -rec.txt, -end.txt (pole dump of her and the checkpoint), -inst.txt,
# -end.png. Compare her trace with the checkpoint instance's position (both tru.exe frame).
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"
: "${OUT:=$OU_WREF_DIR/out}"; mkdir -p "$OUT"
W="$WREF_SRC/w.sh"
O=$OUT/$3
"$WREF_SRC/start-kwin.sh" || exit 1
"$WREF_SRC/newgame.sh" > "$O-newgame.log" 2>&1
P=$("$W" wmem rd C59264 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
echo "level data $P" > "$O-ld.txt"
"$W" wmem rd "$(printf %X $((0x$P + 0x80)))" 384 >> "$O-ld.txt"
U=$(printf %X "$2")
w() { "$W" wmem w32 "$(printf %X $((0x$P + 0x90)))" "$U"; "$W" wmem w32 "$(printf %X $((0x$P + 0x114)))" "$U"; }
w
"$W" wmem goto "$1"
if [ -n "$NOGOD" ]; then "$W" ref rec "${REC_MS:-60000}" > "$O-rec.txt" &
else GOD=1 "$W" ref rec "${REC_MS:-60000}" > "$O-rec.txt" & fi
R=$!
i=0; while [ $i -lt 40 ]; do w; sleep 0.5; i=$((i+1)); done
wait $R
echo "after load" >> "$O-ld.txt"
"$W" wmem rd "$(printf %X $((0x$P + 0x80)))" 384 >> "$O-ld.txt"
"$W" pole dump -1 "$2" > "$O-end.txt"
"$W" wmem inst > "$O-inst.txt"
"$WREF_SRC/shot.sh" "$O-end.png"
"$WREF_SRC/stop.sh"
