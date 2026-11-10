#!/bin/sh
# Wine (heavy.lock + wine.lock): tru.exe's walkable slope limit. l2_junglehub (tru = ours + (8558.23, -19414.44,
# -18073.17)): Lara is put 15 over the centroid of one large face per normal-z bin (find_ramps) and left alone for
# 3 s: on walkable ground she stands, past the limit she slides down the fall line.
P=/mnt/1st4TB/Projects/openunderworld-work/poleattach
B=${OU_EVAL_DIR:-/tmp/mover_eval} # find_ramps built here (tools/mover_eval/find_ramps.cpp)
W=$P/w.sh
O=$B/slope-wine.txt
setsid $P/start-kwin.sh > $B/kwin.log 2>&1 3>&- 4>&- 5>&- 6>&- 7>&- 8>&- 9>&- &
KP=$!
sleep 8
$P/newgame.sh > $B/newgame.log 2>&1
$W wmem goto l2_junglehub; sleep 95
echo "== start junglehub" > $O; $W pole dump -1 >> $O
try() { # nz x y z yaw
    $W pt tp $2 $3 $4 400 >/dev/null; $W pole yaw $5; sleep 0.3
    $W pt tp $2 $3 $4 300 >/dev/null; $W pole yaw $5; sleep 0.2
    echo "== nz $1 at $2 $3 $4" >> $O
    GOD=1 $W pt trace 3000 >> $O 2>&1
    echo "== nz $1 after" >> $O; $W pole dump -1 >> $O
}
$B/find_ramps "/mnt/1st4TB/Tomb Raider Underworld" l2_junglehub 8558.23 -19414.44 -18073.17 | while read a nz b area c ox oy oz d tx ty tz e f yaw; do
    try $nz $tx $ty $(echo "$tz + 15" | bc -l) $yaw < /dev/null
done
pkill -f tru.exe; sleep 2
WINEPREFIX=/mnt/1st4TB/Projects/openunderworld-work/wine wineserver -k 2>/dev/null
systemctl --user stop ou-gear-tru 2>/dev/null
kill -- -$KP 2>/dev/null; kill $KP 2>/dev/null
