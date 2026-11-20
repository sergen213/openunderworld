#!/bin/sh
# One Wine session in l4_temple: the safety drop (walking / running off the block of ledge 1155 with no key, OU-PLY-008).
# tru.exe frame (ours = tru + (-500.70, -9432.76, 19909.81)): ledge z -17381.41, west face x 334.8, north face y
# -1836.0, south face y -2998.8; hang root off the west face (173.65, y, -17875.24). She is put on the floor west of
# the block, jumps to the west face's ledge and pulls up (root 404.6, -2416.6: 70 inside the west face, facing +x),
# then (rec traces, chart request / parent per sample):
#   back.txt    S (back) held: turns round and runs 70 to the west face (the old gameplaygoal/inputrules/wine2.txt run)
#   left.txt    A (left) held: runs ~580 along +y off the north face (full run speed)
#   sprint.txt  D (right) + Shift held: sprints ~580 along -y off the south face
#   walk.txt    Walk (Alt) + S: walks off the west face
# Usage: OU_WREF_DIR=... WINEPREFIX=... OUT=dir sessions/l4_temple_safety_drop.sh
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
: "${OUT:=$OU_WREF_DIR/out}"; mkdir -p "$OUT"
W="$WREF_SRC/w.sh"
R() { "$W" ref "$@"; }
"$WREF_SRC/start-kwin.sh" || exit 1
"$WREF_SRC/newgame.sh" > "$OUT/newgame.log" 2>&1
"$W" wmem goto l4_temple; sleep 100
"$W" pole dump -1 > "$OUT/start.txt"
floor() { # let go of anything (Crouch), then stand on the floor 300 west of the face, facing it
    GOD=1 R trace 300 a2:0:120 > /dev/null; sleep 1.5
    R tp 34.80 -2417.44 -18205 400 > /dev/null; "$W" pole yaw 1.5708; sleep 0.3
    R tp 34.80 -2417.44 -18205 300 > /dev/null; "$W" pole yaw 1.5708; sleep 1.5
}
ontop() { # jump to the ledge, pull up
    floor
    GOD=1 R rec 2600 20:300:450 | tail -2 > "$OUT/$1-jump.txt"
    GOD=1 R rec 3500 20:100:250 | tail -3 > "$OUT/$1-pullup.txt"
}
ontop back;   GOD=1 R rec 5000 53:200:2500 > "$OUT/back.txt"; "$WREF_SRC/shot.sh" "$OUT/back.png"
ontop left;   GOD=1 R rec 5000 41:200:2500 > "$OUT/left.txt"; "$WREF_SRC/shot.sh" "$OUT/left.png"
ontop sprint; GOD=1 R rec 5000 44:200:2500 a0:200:2500 > "$OUT/sprint.txt"; "$WREF_SRC/shot.sh" "$OUT/sprint.png"
ontop walk;   GOD=1 R rec 7000 a4:0:6500 53:200:6500 > "$OUT/walk.txt"; "$WREF_SRC/shot.sh" "$OUT/walk.png"
"$W" pole dump -1 > "$OUT/end.txt"
"$WREF_SRC/stop.sh"
