#!/bin/sh
# SPAWN-free chart takes (Main, after the post-SPAWN carry, README "Carry after SPAWN"): Lara is born on a checkpoint at
# a fresh load of its unit (the checkpoint_respawn.sh method: the checkpoint uid written into the player's level data
# *0xC59264 +0x90 / +0x114 before and during `wmem goto`, 0x5D40C0 births her there), then reaches the target by
# keys only, in closed loop (short key traces, her position read between them), with the camera pinned (CAMFOLLOW
# moves only the camera) so Forward is a known world direction. No SPAWN, no RESET pin.
#   pole: checkpoint 5786 (l4_traprooms, ours (14598.4, -23468.9, 6358.3) facing +y, the floor under pole 1336, tru z
#   -939.8): walk +y until her y is STANDOFF short of 1336's axis (tru y 4946.06), then the takes.
# Writes the probe log $OU_WREF_DIR/birth_<target>.txt; the cases go to the corpus (pole/<case>_birth).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_birth.sh pole
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
W="$WREF_SRC/w.sh"
birth() { # UNIT CHECKPOINT_UID
    cunit l3_main_bedroom || return 1
    P=$("$W" wmem rd C59264 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'); U=$(printf %X "$2")
    wl() { "$W" wmem w32 "$(printf %X $((0x$P + 0x90)))" "$U" > /dev/null; "$W" wmem w32 "$(printf %X $((0x$P + 0x114)))" "$U" > /dev/null; }
    wl; "$W" wmem goto "$1"
    i=0; while [ $i -lt 40 ]; do wl; sleep 0.5; i=$((i+1)); done
    i=0; until "$W" wmem state | grep -qi "unit $1 .*flags 00000000" || [ $i -ge 30 ]; do sleep 10; i=$((i+1)); done
    sleep 20; "$W" wmem state | tr -d '\r'; }
pos() { "$W" chart info | tr -d '\r' | awk '/^frame/{for(i=1;i<=NF;i++) if($i=="pos") print $(i+1), $(i+2), $(i+3)}'; }
case $1 in
pole)
    UNIT=l4_traprooms; OFF=-7342.10,25801.50,-7298.09; L="$OU_WREF_DIR/birth_pole.txt"; CF=0,1,1264,300
    birth $UNIT 5786 > "$L" 2>&1 || exit 1
    echo "born at $(pos)" >> "$L"
    # idle control: 90 ticks, camera pinned, no keys (drift must stay < 5)
    CAMFOLLOW=$CF "$W" chart trace 90 | tr -d '\r' > "$OU_WREF_DIR/birth_pole_idle.tsv"
    echo "after idle $(pos)" >> "$L"
    # walk +y (Alt walk + Up) in 10-tick steps until y >= 4946.06 - STANDOFF, then back (Down) in 4-tick steps until she
    # stands under 1336's axis (y <= 4975): 2026-11-22 an Up + Jump from 420 short climbed a step past the pole instead
    T=$(echo "4946.06 ${STANDOFF:-420}" | awk '{print $1 - $2}'); n=0
    while :; do y=$(pos | awk '{print $2}'); echo "walk y $y target $T" >> "$L"
        awk -v y="$y" -v t="$T" 'BEGIN{exit !(y >= t)}' && break
        n=$((n+1)); [ $n -gt 80 ] && { echo "walk did not arrive" >> "$L"; exit 1; }
        CAMFOLLOW=$CF "$W" chart trace 10 26:0:10 A4:0:10 > /dev/null; done
    CAMFOLLOW=$CF "$W" chart trace 30 26:0:30 A4:0:30 > /dev/null
    n=0; while :; do y=$(pos | awk '{print $2}'); awk -v y="$y" 'BEGIN{exit !(y <= 4975)}' && break
        n=$((n+1)); [ $n -gt 40 ] && break; CAMFOLLOW=$CF "$W" chart trace 4 28:0:4 A4:0:4 > /dev/null; done
    CAMFOLLOW=$CF "$W" chart trace 40 > /dev/null; echo "start $(pos)" >> "$L"
    d="$OU_CHART_DIR/pole"
    take() { # NAME TICKS NOTES KEYS...: a take from where she stands, case.json with spawn = her tick-0 position
        n=$1 ti=$2 no=$3; shift 3
        CAMFOLLOW=$CF OFFSET=$OFF "$W" chart trace "$ti" "$@" | tr -d '\r' > "$d/$n.tsv"
        S=$(awk -F'\t' 'NR==2{for(i=14;i<=NF;i++){if($i~/var_tpx=/)x=substr($i,9); if($i~/var_tpy=/)y=substr($i,9); if($i~/var_tpz=/)z=substr($i,9); if($i~/var_tyaw=/)w=substr($i,10)}; print x","y","z","w}' "$d/$n.tsv")
        python3 "$CHART_PY" write "$d/$n.case.json" --unit $UNIT --offset "$OFF" --spawn "$S" --hold 0 --settle 0 --ticks "$ti" \
            --cam "$CF" --notes "NO SPAWN: born on checkpoint 5786 at a fresh l4_traprooms load, reached the floor under pole 1336 by keys (spawn = her position at tick 0); $no" -- "$@"
        python3 - "$d/$n.case.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c['spawn']['method'] = 'checkpoint birth (uid 5786, fresh l4_traprooms load) + keys only; replays must not SPAWN'
json.dump(c, open(sys.argv[1], 'w'), indent=1)
PY
        python3 "$CHART_PY" summary "$d/$n.tsv" | tee -a "$L" | cut -c1-400; }
    take tr_pole_drop_birth 400 "Jump 10..14 (standing jump up), air grab of 1336, hang, Crouch 250..254: drop to the floor" 20:10:14 11:250:254
    take tr_pole_top_jump_up_birth 480 "Jump 10..14: standing jump up to 1336, air grab; Jump 160..164, no stick: onto the pole top; Jump 320..324: straight up, back on top" 20:10:14 20:160:164 20:320:324
    CAMFOLLOW=$CF "$W" chart trace 120 11:20:24 > /dev/null   # Crouch: from the hang back to the floor under the pole
    take tr_pole_top_jump_fwd_birth 520 "Jump 10..14: standing jump up to 1336, air grab; Jump 160..164: onto the pole top; Up 320..360 + Jump 324..328: jump forward (+y, toward pole 1337)" 20:10:14 20:160:164 26:320:360 20:324:328 ;;
esac
