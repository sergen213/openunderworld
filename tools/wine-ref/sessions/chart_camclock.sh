#!/bin/sh
# Camera look-idle clock after a checkpoint birth (TraversalCam): g_camera = *(0xE80534) +0x1E4 (float s; the reset
# 0x5FF7F0 sets settings +0xC4 + +0xC8), its vftable (0x85600C = LaraCameraRewrite), +0x1D4, +0x1320, the frame and
# timeMult (0xE7F188) every tick, no SPAWN, no camera pin. `wmem goto l4_traprooms` with checkpoint 5786 in the
# player's level data (chart_birth.sh); the trace starts as soon as the unit is up with gameTracker flags 0 (a
# lockstep trace across the load stalls it, 2026-11-22), so the clock's first rows are ~1-2 s after the birth: the
# frame column dates them. Then a second goto of the same unit, the same way.
# Writes $OU_WREF_DIR/camclock_birth.tsv and camclock_regoto.tsv (600 ticks = 10 s each).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_camclock.sh
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
W="$WREF_SRC/w.sh"
export VARS=cam:a:E80534,cam_vt:m:0,clock:m:1E4:f,dist_state:m:1D4:f,target_dist:m:1320:f,timemult:a:E7F188:f GOD=1
cunit l3_main_bedroom || exit 1
i=0; until "$W" wmem state | tr -d '\r' | grep -q " flags 00000000 " || [ $i -ge 60 ]; do sleep 5; i=$((i+1)); done
P=$("$W" wmem rd C59264 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'); U=$(printf %X 5786)
wl() { "$W" wmem w32 "$(printf %X $((0x$P + 0x90)))" "$U" > /dev/null; "$W" wmem w32 "$(printf %X $((0x$P + 0x114)))" "$U" > /dev/null; }
for part in birth regoto; do
    wl; f0=$("$W" chart info | tr -d '\r' | awk '/^frame/{print $2}')
    "$W" wmem goto l4_traprooms
    i=0; until "$W" wmem state | tr -d '\r' | grep -q "unit l4_traprooms .*flags 00000000\|flags 00000000 unit l4_traprooms" || [ $i -ge 300 ]; do
        [ $i -lt 40 ] && wl; sleep 0.5; i=$((i+1)); done
    echo "$part: goto at frame $f0, unit up after ~$((i / 2)) s: $("$W" wmem state | tr -d '\r')"
    "$W" chart trace 600 | tr -d '\r' > "$OU_WREF_DIR/camclock_$part.tsv"
done
