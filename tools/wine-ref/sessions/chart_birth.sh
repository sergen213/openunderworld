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
    # 2026-11-22: a goto while the prologue cinematic still runs (gameTracker flag 1) never happens: wait it out first
    i=0; until "$W" wmem state | tr -d '\r' | grep -q " flags 00000000 " || [ $i -ge 60 ]; do sleep 5; i=$((i+1)); done
    P=$("$W" wmem rd C59264 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/'); U=$(printf %X "$2")
    wl() { "$W" wmem w32 "$(printf %X $((0x$P + 0x90)))" "$U" > /dev/null; "$W" wmem w32 "$(printf %X $((0x$P + 0x114)))" "$U" > /dev/null; }
    wl; "$W" wmem goto "$1"
    i=0; while [ $i -lt 40 ]; do wl; sleep 0.5; i=$((i+1)); done
    i=0; until "$W" wmem state | grep -qi "unit $1 .*flags 00000000" || [ $i -ge 30 ]; do sleep 10; i=$((i+1)); done
    sleep 20; "$W" wmem state | tr -d '\r'; }
pos() { "$W" chart info | tr -d '\r' | awk '/^frame/{for(i=1;i<=NF;i++) if($i=="pos") print $(i+1), $(i+2), $(i+3)}'; }
case $1 in
pole|polecd)
    UNIT=l4_traprooms; OFF=-7342.10,25801.50,-7298.09; L="$OU_WREF_DIR/birth_pole.txt"; CF=0,1,1264,300
    birth $UNIT 5786 > "$L" 2>&1 || exit 1
    echo "born at $(pos)" >> "$L"
    # idle control: 90 ticks, camera pinned, no keys (drift must stay < 5)
    CAMFOLLOW=$CF "$W" chart trace 90 | tr -d '\r' > "$OU_WREF_DIR/birth_pole_idle.tsv"
    echo "after idle $(pos)" >> "$L"
    # 2026-11-22: on the x 7256 line something pushes her back ~650 near y 4100-4300 (4 runs); tr_pole_drop_birth started at
    # x 7515: walk +x (camera 1,0) to x >= 7500 first, then +y
    n=0; while :; do x=$(pos | awk '{print $1}'); echo "walk x $x" >> "$L"
        awk -v x="$x" 'BEGIN{exit !(x >= 7500)}' && break
        n=$((n+1)); [ $n -gt 30 ] && break
        CAMFOLLOW=1,0,1264,300 "$W" chart trace 10 26:0:10 A4:0:10 > /dev/null; done
    CAMFOLLOW=$CF "$W" chart trace 40 > /dev/null
    # walk +y (Alt walk + Up) in 10-tick steps until y >= T - 10, T = 4946.06 - STANDOFF (default 0: under 1336's axis,
    # where tr_pole_drop_birth started; 2026-11-22 an Up + Jump from 420 short climbed a step past the pole, Down steps
    # back turned her round and overshot, and 3-tick steps never got her walking)
    T=$(echo "4946.06 ${STANDOFF:-0}" | awk '{print $1 - $2}'); n=0
    while :; do y=$(pos | awk '{print $2}'); echo "walk y $y target $T" >> "$L"
        awk -v y="$y" -v t="$T" 'BEGIN{exit !(y >= t - 10)}' && break
        n=$((n+1)); [ $n -gt 120 ] && { echo "walk did not arrive" >> "$L"; exit 1; }
        # past y 3900 a walk (Alt) turns back at an edge near y 4150 (2026-11-22: pushed back ~650 three times); run there
        if awk -v y="$y" 'BEGIN{exit !(y >= 3900)}'; then K="26:0:6"; k=6; else K="26:0:10 A4:0:10"; k=10; fi
        CAMFOLLOW=$CF "$W" chart trace $k $K > /dev/null; done
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
    if [ "$1" = polecd ]; then
        # ChartCD / ChartJ: tr_pole_drop_birth's jump again, CALLFN on 0x7234A0 (class test kind 13: [arg1] node, +8 hit
        # time, arg3 threshold, AL result) and the player data's markup-search fields; then the ids of the logged nodes
        n=tr_pole_drop_birth_cd
        CALLS=1 CALLFN=7234A0:1:R1 VARS=ac0:d:AC0,a64:d:A64,a68:d:A68:f,f77c:d:77C:f CAMFOLLOW=$CF OFFSET=$OFF "$W" chart trace 60 20:10:14 \
            | tr -d '\r' > "$d/$n.tsv"
        python3 "$CHART_PY" summary "$d/$n.tsv" | tee -a "$L" | cut -c1-300
        for ptr in $(grep -o 'var_[a-z0-9]*=[^	]*' "$d/$n.tsv" | grep -o '7234a0[^;]*' | awk -F: '{print $8}' | sort -u) \
                   $(awk -F'\t' 'NR>1{for(i=14;i<=NF;i++) if($i~/^var_a64=/) print substr($i,9)}' "$d/$n.tsv" | sort -u); do
            [ "$ptr" = 0 ] && continue
            mk=$("$W" wmem rd "$(printf %X $((0x$ptr + 0x20)))" 4 | tr -d ' \r\n' | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
            id=$("$W" wmem rd "$(printf %X $((0x$mk + 0x2E)))" 2 | tr -d ' \r\n' | sed 's/\(..\)\(..\)/\2\1/')
            own=$("$W" wmem rd "$(printf %X $((0x$ptr + 0x2E)))" 2 | tr -d ' \r\n' | sed 's/\(..\)\(..\)/\2\1/')
            echo "ptr $ptr: [+0x20] $mk -> id(+0x2E) $((0x$id)); own +0x2E $((0x$own))" | tee -a "$OU_WREF_DIR/$n.ids.txt"; done
        exit 0
    fi
    take tr_pole_drop_birth 400 "Jump 10..14 (standing jump up), air grab of 1336, hang, Crouch 250..254: drop to the floor" 20:10:14 11:250:254
    take tr_pole_top_jump_up_birth 480 "Jump 10..14: standing jump up to 1336, air grab; Jump 160..164, no stick: onto the pole top; Jump 320..324: straight up, back on top" 20:10:14 20:160:164 20:320:324
    CAMFOLLOW=$CF "$W" chart trace 120 11:20:24 > /dev/null   # Crouch: from the hang back to the floor under the pole
    take tr_pole_top_jump_fwd_birth 520 "Jump 10..14: standing jump up to 1336, air grab; Jump 160..164: onto the pole top; Up 320..360 + Jump 324..328: jump forward (+y, toward pole 1337)" 20:10:14 20:160:164 26:320:360 20:324:328 ;;
stickjump)
    # Main / ChartCD / ChartJ: take-off state for Forward + Jump from idle, 5 trials with both on the same tick and 5
    # with Forward one tick earlier; per tick the stick record *(*(0xFADFB0)+0xB0) +0x18 (magnitude, 0x706540). Born on
    # traprooms checkpoint 5786 (flat floor, idle drift 0); the camera direction alternates per trial (+y / -y) so she
    # jumps back and forth instead of away. Writes $OU_WREF_DIR/stickjump/<same|early><n>.tsv
    UNIT=l4_traprooms; L="$OU_WREF_DIR/birth_stickjump.txt"; mkdir -p "$OU_WREF_DIR/stickjump"
    birth $UNIT 5786 > "$L" 2>&1 || exit 1
    echo "born at $(pos)" >> "$L"
    n=0; for mode in same early same early same early same early same early; do n=$((n+1))
        [ $((n % 2)) = 1 ] && CF=0,1,1264,300 || CF=0,-1,1264,300
        CAMFOLLOW=$CF "$W" chart trace 90 > /dev/null   # settle to idle under the new camera
        [ $mode = same ] && K="26:20:50 20:20:24" || K="26:19:50 20:20:24"
        CAMFOLLOW=$CF VARS=sm:s:18:f,s100:s:100:f,s110:s:110:f "$W" chart trace 80 $K | tr -d '\r' > "$OU_WREF_DIR/stickjump/$mode$n.tsv"
        CAMFOLLOW=$CF "$W" chart trace 120 > /dev/null; echo "trial $n $mode at $(pos)" >> "$L"; done ;;
ladder*)
    # l7_arctic_sea ladder 2044 (ours plane y -47420.7, x 16355.6..16513.6, z -19461.1..-17318.5) across the shaft whose
    # near edge is at ours y ~ -46500 (floor -17634.6, our collision, ours/floorline.cpp): born on checkpoint 9223 (ours
    # (16454.3, -44532.2, -17634.6) facing +y), walk -y until RUNUP short of the edge, then a running jump onto the
    # ladder; camera pinned behind her (forward = -y) until a ladder state (CAMUNTIL), then free, CAMDUMP + STICKDUMP.
    #   ladder_up: then Up held (climb); ladder_side: then Left held 140.. (side).
    UNIT=l7_arctic_sea; OFF=-5610.33,22439.56,19571.30; L="$OU_WREF_DIR/birth_$1.txt"; CF=0,-1,1264,300
    birth $UNIT 9223 > "$L" 2>&1 || exit 1
    echo "born at $(pos)" >> "$L"
    CAMFOLLOW=$CF "$W" chart trace 90 | tr -d '\r' > "$OU_WREF_DIR/birth_$1_idle.tsv"; echo "after idle $(pos)" >> "$L"
    T=$(echo "-46500 ${RUNUP:-420}" | awk '{print $1 + $2 + 22439.56}'); n=0   # tru y to stop at
    while :; do y=$(pos | awk '{print $2}'); echo "walk y $y target $T" >> "$L"
        awk -v y="$y" -v t="$T" 'BEGIN{exit !(y <= t)}' && break
        n=$((n+1)); [ $n -gt 80 ] && { echo "walk did not arrive" >> "$L"; exit 1; }
        CAMFOLLOW=$CF "$W" chart trace 10 26:0:10 A4:0:10 > /dev/null; done
    CAMFOLLOW=$CF "$W" chart trace 40 > /dev/null; echo "start $(pos)" >> "$L"
    d="$OU_CHART_DIR/ladder"; n=lad_a2044_$1_birth
    case $1 in ladder_up) K="26:0:520 20:${JUMPAT:-16}:$((${JUMPAT:-16} + 4))" ;; *) K="26:0:60 20:${JUMPAT:-16}:$((${JUMPAT:-16} + 4)) 25:140:380" ;; esac
    CAMDUMP=1 STICKDUMP=416 CAMUNTIL=1066,1084,1083,1053,1055,1085,1087 CAMFOLLOW=$CF OFFSET=$OFF "$W" chart trace 560 $K | tr -d '\r' > "$d/$n.tsv"
    S=$(awk -F'\t' 'NR==2{for(i=14;i<=NF;i++){if($i~/var_tpx=/)x=substr($i,9); if($i~/var_tpy=/)y=substr($i,9); if($i~/var_tpz=/)z=substr($i,9); if($i~/var_tyaw=/)w=substr($i,10)}; print x","y","z","w}' "$d/$n.tsv")
    python3 "$CHART_PY" write "$d/$n.case.json" --unit $UNIT --offset "$OFF" --spawn "$S" --hold 0 --settle 0 --ticks 560 --cam "$CF" \
        --notes "NO SPAWN: born on checkpoint 9223 at a fresh l7_arctic_sea load, walked -y to ${RUNUP:-420} short of the shaft edge (spawn = her position at tick 0); run + Jump at ${JUMPAT:-16} onto ladder 2044; camera pinned (0,-1) until the first ladder state, then free (CAMUNTIL), CAMDUMP + STICKDUMP" -- $K
    python3 - "$d/$n.case.json" "$d/$n.tsv" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c['spawn']['method'] = 'checkpoint birth (uid 9223, fresh l7_arctic_sea load) + keys only; replays must not SPAWN'
rel = next((l.split('\t')[0] for l in open(sys.argv[2]).read().split('\n')[1:] if 'var_campin=0' in l), None)
if rel is not None: c['camera_released_tick'] = int(rel)
json.dump(c, open(sys.argv[1], 'w'), indent=1)
PY
    python3 "$CHART_PY" summary "$d/$n.tsv" | tee -a "$L" | cut -c1-400 ;;
esac
