# Sourced by the chart_*.sh sessions: record lockstep chart traces (src/chart.c) into the reference corpus
# $OU_CHART_DIR/<family>/<name>.tsv with a sibling <name>.case.json (py/chart_case.py), contract in
# W/chart/wine/README.md. The caller has sourced env.sh (through w.sh) and set UNIT and OFF (tru = ours + OFF, "dx,dy,dz").
#   ccase FAMILY NAME "x,y,z,yaw" (tru frame) TICKS "camdx,camdy" "notes" vk:down:up ...   (key times in ticks)
# Each case first runs an unrecorded reset (RESET ticks, default 150: pinned at the spawn for HOLD ticks, then free with
# RESETKEYS, default Crouch 5..9 = let go): a hang survives a teleport (she stays attached to her ledge), so the reset
# drops her out of it; a fall left by the previous case has ended in the idle before the recorded hold begins.
# Env per case: HOLD (ticks pinned at the spawn, default 30), SETTLE (free frames after the pin before tick 0, not
# recorded, default 0: tick 0 then starts in the idle the landing ends in), CAMDIST / CAMH (default 1264 / 300), VARS.
: "${OU_CHART_DIR:=$OU_WORK/chart/wine}"
CHART_PY="$WREF_SRC/py/chart_case.py"
ccase() {
    fam=$1; name=$2; sp=$3; ticks=$4; cam=$5; notes=$6; shift 6
    d="$OU_CHART_DIR/$fam"; mkdir -p "$d"
    cf=""; [ -n "$cam" ] && cf="$cam,${CAMDIST:-1264},${CAMH:-300}"
    [ "${RESET:-150}" -gt 0 ] && SPAWN="$sp" HOLD="${HOLD:-30}" CAMFOLLOW="$cf" "$WREF_SRC/w.sh" chart trace "${RESET:-150}" \
        ${RESETKEYS-11:5:9} > /dev/null
    SPAWN="$sp" HOLD="${HOLD:-30}" SETTLE="${SETTLE:-0}" OFFSET="$OFF" CAMFOLLOW="$cf" "$WREF_SRC/w.sh" chart trace "$ticks" "$@" \
        | tr -d '\r' > "$d/$name.tsv"
    python3 "$CHART_PY" write "$d/$name.case.json" --unit "$UNIT" --offset "$OFF" --spawn "$sp" --hold "${HOLD:-30}" \
        --settle "${SETTLE:-0}" --ticks "$ticks" --cam "$cf" --notes "$notes" -- "$@"
    python3 "$CHART_PY" summary "$d/$name.tsv" | cut -c1-600
}
# Position check after a goto: prints the player position; returns 1 if she is not near "x,y" (tru) within 20000.
cnear() {
    "$WREF_SRC/w.sh" wmem state | tr -d '\r' | awk -v xy="$1" 'BEGIN{split(xy,a,",")} {print}
        / pos /{for(i=1;i<=NF;i++) if($i=="pos"){dx=$(i+1)-a[1]; dy=$(i+2)-a[2]; if(dx*dx+dy*dy>4e8) exit 1}}'
}
# cunit UNIT: a fresh session in UNIT (README: only the first `wmem goto` of a session is reliable): stop, compositor,
# tru.sh, the front end driven by its state (not newgame.sh's fixed key list, whose presses were lost or landed in the
# attract movie on a loaded machine), goto, wait for the load. Prints the start position.
cunit() {
    "$WREF_SRC/stop.sh"; "$WREF_SRC/start-kwin.sh" > /dev/null || return 1
    # The private prefix's autosave is the last checkpoint of the previous session (a goto unit): with it the centre item
    # is RESUME GAME, which loads that unit instead of the prologue. Remove it (inside the prefix only; wref_private_prefix
    # has checked that no user directory links out) so the front end offers PLAY GAME again.
    wref_private_prefix; rm -f "$WINEPREFIX"/drive_c/users/*/Documents/Eidos/"Tomb Raider - Underworld"/Autosave.TRUSave
    "$WREF_SRC/tru.sh" || return 1
    # Setup dialog (if it opens): Ok. Main state 6 = "Create autosave file?" (NO selected: Left, Return); front-end
    # sub-state 7 = the attract movie after ~60 s idle (Escape); the main menu (main 10, sub-state 3): Return = PLAY GAME,
    # then Return on the difficulty (Tomb Raider selected). Up to ~10 min on a loaded machine.
    i=0; until "$WREF_SRC/w.sh" wmem state 2>/dev/null | grep -q "^main 1 " || [ $i -ge 60 ]; do
        "$WREF_SRC/w.sh" wkey click "Tomb Raider: Underworld - Setup" Ok > /dev/null 2>&1
        st=$("$WREF_SRC/w.sh" wmem state 2>/dev/null | tr -d '\r')
        case "$st" in
            "main 6 "*) "$WREF_SRC/w.sh" wkey key 25 200 > /dev/null; sleep 2; "$WREF_SRC/w.sh" wkey key 0D 200 > /dev/null ;;
            "main 10 fe 7 "*) "$WREF_SRC/w.sh" wkey key 1B 200 > /dev/null ;;
            "main 10 fe 3 "*) "$WREF_SRC/w.sh" wkey key 0D 200 > /dev/null ;;
        esac
        sleep 10; i=$((i+1)); done
    "$WREF_SRC/w.sh" wmem state | grep -qi "unit l3_main_bedroom" || { echo "new game did not reach the prologue" >&2; return 1; }
    [ "$1" = l3_main_bedroom ] && return 0
    sleep 10; "$WREF_SRC/w.sh" wmem goto "$1"; sleep 100
    i=0; until "$WREF_SRC/w.sh" wmem state | grep -qi "unit $1 " || [ $i -ge 12 ]; do sleep 10; i=$((i+1)); done
    "$WREF_SRC/w.sh" wmem state | tr -d '\r' | grep -i "unit $1 " || { echo "goto $1 failed" >&2; return 1; }
}
# cground: ccase with SETTLE (default 90) free frames before tick 0, for cases that start standing (the pin's landing
# has ended in the idle); cases that start with a fall or catch (hangs, holds, ladders) use ccase.
cground() { _s=${SETTLE:-0}; SETTLE=${GSETTLE:-90}; ccase "$@"; SETTLE=$_s; }
