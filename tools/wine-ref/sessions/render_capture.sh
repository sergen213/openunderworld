#!/bin/sh
# D3D9 call capture of tru.exe for docs/re/RENDER_PIPELINE.md and docs/REFERENCE_COMPARISON.md "Rendering".
# apitrace's d3d9.dll wrapper (apitrace 14.0 win32, lib/wrappers/d3d9.dll) sits next to tru.exe in OU_TRU_DIR and
# forwards to the prefix's DXVK d3d9.dll (system directory); every D3D9 call of the session goes into $OUT/tru.trace.
# The scene is held still for the last seconds, so the trace's last complete frames are the scene's frames
# (py/d3d9_frames.py reduces them). One scene per session (only the first `wmem goto` of a session is reliable).
#   SCENE=bedroom     new game, the prologue's first gameplay view in l3_main_bedroom (prologue frame: ours + (9622,
#                     333, -2977)), Lara idle
#   SCENE=cliffclimb  new game, then `wmem goto l2_cliffclimb`: Lara at the unit's start on the yacht, idle
#   SCENE=sharks      as cliffclimb, then Lara dropped treading at the open-water point of l2_cliffclimb_sharks.sh;
#                     the sharks circle her (batch LODs of shark_blueshark at changing distances, py/d3d9_frames.py lod)
# Writes into $OUT: tru.trace, newgame.log, cam.txt (wmem cam), rec.txt (ref rec: camera matrix rows `mx`, look
# `lk`), shot.png (1280x720); sharks: enemies.txt, watch.txt (enemy watch: shark positions every 100 ms over the
# trace's last 20 s), camwatch.txt (wmem cam over the same time).
# Usage: OU_WREF_DIR=... WINEPREFIX=<private prefix copy> OU_TRU_DIR=<private game copy with the wrapper> \
#        OU_WREF_CPUS=4-7 OUT=dir SCENE=... sessions/render_capture.sh
# (a private prefix and game copy on NVMe need no wine.lock; the shared $OU_WORK/wine prefix does).
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
: "${OUT:=$OU_WREF_DIR/out}"; : "${SCENE:=bedroom}"; mkdir -p "$OUT"
[ -f "$OU_TRU_DIR/d3d9.dll" ] || { echo "no apitrace d3d9.dll wrapper in $OU_TRU_DIR" >&2; exit 1; }
W="$WREF_SRC/w.sh"
rm -f "$OUT/tru.trace"
TRACE_FILE="Z:$(printf '%s' "$OUT/tru.trace" | tr / '\\')"; export TRACE_FILE
"$WREF_SRC/start-kwin.sh" || exit 1
"$WREF_SRC/newgame.sh" > "$OUT/newgame.log" 2>&1
still() { # prefix: camera, matrix rows and a screenshot of the settled view, then keep it still for the trace's tail
    sleep "${SETTLE:-20}"
    "$W" wmem cam 1 > "$OUT/$1cam.txt" 2>&1
    "$W" ref rec 300 > "$OUT/$1rec.txt" 2>&1
    "$WREF_SRC/shot.sh" "$OUT/$1shot.png"
    sleep 6
}
case $SCENE in
bedroom) still "" ;;
cliffclimb)
    "$W" wmem goto l2_cliffclimb; sleep 100
    still "" ;;
sharks)
    "$W" wmem goto l2_cliffclimb; sleep 100
    "$W" enemy list > "$OUT/enemies.txt"
    UIDS=$(awk '$NF ~ /shark/ {print $1}' "$OUT/enemies.txt" | tr '\n' ' ')
    "$W" ref drop -19500 45500 -18939.82 500 3000 0 0 0 > /dev/null   # 44 under the sea volume's top (-18895.82)
    sleep 40                                                            # the sharks come round
    "$W" wmem cam 200 100 > "$OUT/camwatch.txt" 2>&1 &
    "$W" enemy watch 20000 100 $UIDS > "$OUT/watch.txt" 2>&1
    wait; "$WREF_SRC/shot.sh" "$OUT/shot.png"; sleep 1 ;;
*) echo "SCENE wants bedroom, cliffclimb or sharks" >&2 ;;
esac
"$WREF_SRC/stop.sh"
ls -l "$OUT"
