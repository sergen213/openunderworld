#!/bin/sh
# Ground-to-pole jumps at l4_traprooms pole 1336 (ChartJ: the in-flight jump-target refine against Wine). The pole axis
# runs along x at tru (y 4947.4, z -165.6) (ours y -20854.6, z 7132.6). GameReal's floor spot ours (14300, -21328, 6400)
# = tru (6958.1, 4474, -898.2) is 473 short in y, and a standing jump from there misses in tru.exe (chart_l4_traprooms.sh).
# So each case walks the start toward the pole in 60-unit steps (settled on the floor, GSETTLE) until the take reaches
# the pole idle S623. Takes that never reach it stay under wallhug_attempt/.
# Columns: ANIM=1, plus the flight fields pd +0x77C (time left, f), +0xA64 (jump-target markup), +0xA68 (its param, f),
# +0x9F0..+0x9F8 (its point, f), +0xC20 / +0xC24 (f).
# Cases (pole/):
#   tr_pole_ground_stand_jump  stick jump: Up + Jump from standing
#   tr_pole_ground_run_jump    running jump: Up held from 600 back, Jump on approach
#   tr_pole_ground_fling       standing take, then swing + Jump to pole 1337
# Usage (session dir exported, see chart_lib.sh): sessions/chart_poleground.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7341.9,25802.0,-7298.2
[ "$1" = noload ] || cunit $UNIT || exit 1
export ANIM=1 VARS=pd_77c:pd:77C:f,pd_a64:pd:A64,pd_a68:pd:A68:f,pd_9f0:pd:9F0:f,pd_9f4:pd:9F4:f,pd_9f8:pd:9F8:f,pd_c20:pd:C20:f,pd_c24:pd:C24:f
CP=0,1; X=6958.1; Z=-890; YAW=3.14159
reached() { awk -F'\t' 'NR>1 && $2==623{f=1} END{exit !f}' "$OU_CHART_DIR/pole/$1.tsv"; }
park() { mkdir -p "$OU_CHART_DIR/wallhug_attempt"; for e in tsv case.json; do mv "$OU_CHART_DIR/pole/$1.$e" "$OU_CHART_DIR/wallhug_attempt/$1.$e"; done; }
# ChartJ: G = *(0xE7EE50); float G+0x184 (markup pitch limit 0x733F60 -> 0x733EC0), into $OU_WREF_DIR/pitchlimit.txt
le32() { tr -s ' \r\n' ' ' | awk '{printf "%s%s%s%s", $4, $3, $2, $1}'; }
G=$("$WREF_SRC/w.sh" wmem rd E7EE50 4 | le32)
V=$("$WREF_SRC/w.sh" wmem rd "$(printf %x $((0x$G + 0x184)))" 4 | le32)
python3 -c "import struct;print('G %s G+0x184 %s = %r' % ('$G', '$V', struct.unpack('<f', bytes.fromhex('$V'))[0]))" | tee "$OU_WREF_DIR/pitchlimit.txt"
# ChartJ: tr_pole_grab again with the layer's +0xF4 flags (bit 0x20 = grab side, 0x74C8D0), player +0xED0, +0xCB0..+0xCBC
VARS=L_f4:L:F4,pd_ed0:pd:ED0,pd_cb0:pd:CB0:f,pd_cb4:pd:CB4:f,pd_cb8:pd:CB8:f,pd_cbc:pd:CBC:f \
    creplay "$OU_CHART_DIR/pole/tr_pole_grab.case.json" tr_pole_grab_side
probe() { n=$1 ti=$2 y=$3 dy=$4 cnt=$5 no=$6; shift 6; k=0
    while [ $k -lt "$cnt" ]; do
        cground pole "$n" "$X,$y,$Z,$YAW" "$ti" "$CP" "$no (start tru y $y)" "$@"
        reached "$n" && { echo "$n reached the pole from y $y"; return 0; }
        y=$(echo "$y $dy" | awk '{print $1 + $2}'); k=$((k+1)); done
    echo "$n never reached the pole" >&2; park "$n"; return 1; }
probe tr_pole_ground_stand_jump 200 4474 60 6 "stick jump (Up + Jump at tick 10) from standing toward pole 1336" 26:10:60 20:10:14
probe tr_pole_ground_run_jump 260 3874 60 6 "running jump toward pole 1336: Up held 10..120, Jump 50..54" 26:10:120 20:50:54
# the fling from a ground take: the stand-jump start that worked, then swing (Up 140..340) and Jump 260..264 (M07 timing)
Y=$(python3 -c "import json;print(json.load(open('$OU_CHART_DIR/pole/tr_pole_ground_stand_jump.case.json'))['spawn']['tru'][1])" 2>/dev/null)
[ -n "$Y" ] && probe tr_pole_ground_fling 480 "$Y" 0 1 "stick jump onto pole 1336, swing (Up 140..340), Jump 260..264: fling to pole 1337" \
    26:10:60 20:10:14 26:140:340 20:260:264
