# Sourced by the chart_*.sh sessions: record lockstep chart traces (src/chart.c) into the reference corpus
# $OU_CHART_DIR/<family>/<name>.tsv with a sibling <name>.case.json (py/chart_case.py), contract in
# W/chart/wine/README.md. The caller has sourced env.sh (through w.sh) and set UNIT and OFF (tru = ours + OFF, "dx,dy,dz").
#   ccase FAMILY NAME "x,y,z,yaw" (tru frame) TICKS "camdx,camdy" "notes" vk:down:up ...   (key times in ticks)
# Each case first runs an unrecorded reset (RESET ticks, default 150: pinned at the spawn for HOLD ticks, then free with
# RESETKEYS, default Crouch 5..9 = let go): a hang survives a teleport (she stays attached to her ledge), so the reset
# drops her out of it; a fall left by the previous case has ended in the idle before the recorded hold begins.
# Env per case: HOLD (ticks pinned at the spawn, default 30), CAMDIST / CAMH (default 1264 / 300), VARS (extra columns).
: "${OU_CHART_DIR:=$OU_WORK/chart/wine}"
CHART_PY="$WREF_SRC/py/chart_case.py"
ccase() {
    fam=$1; name=$2; sp=$3; ticks=$4; cam=$5; notes=$6; shift 6
    d="$OU_CHART_DIR/$fam"; mkdir -p "$d"
    cf=""; [ -n "$cam" ] && cf="$cam,${CAMDIST:-1264},${CAMH:-300}"
    [ "${RESET:-150}" -gt 0 ] && SPAWN="$sp" HOLD="${HOLD:-30}" CAMFOLLOW="$cf" "$WREF_SRC/w.sh" chart trace "${RESET:-150}" \
        ${RESETKEYS-11:5:9} > /dev/null
    SPAWN="$sp" HOLD="${HOLD:-30}" OFFSET="$OFF" CAMFOLLOW="$cf" "$WREF_SRC/w.sh" chart trace "$ticks" "$@" \
        | tr -d '\r' > "$d/$name.tsv"
    python3 "$CHART_PY" write "$d/$name.case.json" --unit "$UNIT" --offset "$OFF" --spawn "$sp" --hold "${HOLD:-30}" \
        --ticks "$ticks" --cam "$cf" --notes "$notes" -- "$@"
    python3 "$CHART_PY" summary "$d/$name.tsv" | cut -c1-600
}
# Position check after a goto: prints the player position; returns 1 if she is not near "x,y" (tru) within 20000.
cnear() {
    "$WREF_SRC/w.sh" wmem state | tr -d '\r' | awk -v xy="$1" 'BEGIN{split(xy,a,",")} {print}
        / pos /{for(i=1;i<=NF;i++) if($i=="pos"){dx=$(i+1)-a[1]; dy=$(i+2)-a[2]; if(dx*dx+dy*dy>4e8) exit 1}}'
}
# cunit UNIT: a fresh session in UNIT (README: only the first `wmem goto` of a session is reliable): stop, compositor,
# new game (presses Return again while the front end still shows), goto, wait for the load. Prints the start position.
cunit() {
    "$WREF_SRC/stop.sh"; "$WREF_SRC/start-kwin.sh" > /dev/null || return 1
    "$WREF_SRC/newgame.sh" > "$OU_WREF_DIR/newgame.log" 2>&1
    i=0; until "$WREF_SRC/w.sh" wmem state | grep -q "^main 1 " || [ $i -ge 8 ]; do
        "$WREF_SRC/w.sh" wkey key 0D 250 > /dev/null; sleep 30; i=$((i+1)); done
    "$WREF_SRC/w.sh" wmem state | grep -qi "unit l3_main_bedroom" || { echo "new game did not reach the prologue" >&2; return 1; }
    [ "$1" = l3_main_bedroom ] && return 0
    sleep 10; "$WREF_SRC/w.sh" wmem goto "$1"; sleep 100
    i=0; until "$WREF_SRC/w.sh" wmem state | grep -qi "unit $1 " || [ $i -ge 12 ]; do sleep 10; i=$((i+1)); done
    "$WREF_SRC/w.sh" wmem state | tr -d '\r' | grep -i "unit $1 " || { echo "goto $1 failed" >&2; return 1; }
}
