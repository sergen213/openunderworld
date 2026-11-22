#!/bin/sh
# Stick-controller dump during hangs (ChartA, the 0x195 frame rule): replays with STICKDUMP=0x1A0, i.e. a column
# var_stick=<controller address>:<hex bytes +0x00..+0x19F> of *(*(0xFADFB0)+0xB0) every tick, plus the mode bytes and
# floats ChartA named as VARS (s base). Cases: ledge/ch_hang_left_hold (l3_chapel, camera facing her), l3_main_bedroom
# ledge/hang_shimmy_right and ledge/hang_grab (camera behind her). Files: ledge/<case>_stick.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_stick.sh [chapel|bedroom]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
# CAMDUMP=1 (chart.c): byte 0xE8EEC8, the matrices 0x5E7850 / 0x5E7870 return (var_cam38 / var_cam3c = address:16
# floats), floats 0x8AC298 / 0x8B0444 / 0x8B0448, for ChartA's camera-space check of +0x100 / +0x110.
export STICKDUMP=416 CAMDUMP=1 VARS=s_b0:s:B0,s_dc:s:DC,s_194:s:194,s_b4:s:B4:f,s_d8:s:D8:f
C="$OU_CHART_DIR/ledge"
for part in ${*:-chapel bedroom}; do
    case $part in
    chapel)
        UNIT=l3_chapel; OFF=3196.89,-2412.27,3634.58
        cunit l3_chapel || exit 1
        creplay "$C/ch_hang_left_hold.case.json" ch_hang_left_hold_stick ;;
    bedroom)
        UNIT=l3_main_bedroom; OFF=9622,333,-2977
        cunit l3_main_bedroom || exit 1
        S=1137.7,-3997,-2733,0; i=0   # warm-up: the first jumps after a new game miss ledge 973
        until SPAWN=$S HOLD=60 CAMFOLLOW=0,-1,1264,300 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
            i=$((i+1)); [ $i -gt 20 ] && { echo "ledge 973 never caught" >&2; exit 1; }
            SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null
        creplay "$C/hang_shimmy_right.case.json" hang_shimmy_right_stick
        creplay "$C/hang_grab.case.json" hang_grab_stick ;;
    temple)
        # ChartA: runjump/t1204_run_jump_j50 / j55 (same case files: j50 takes T151, j55..j75 T149) with the stick
        # dump and her yaw *(Instance+0x38), 0xFADF90 (the 0x6F7F40 yaw) and 0xFADF94 as exact bits.
        UNIT=l4_temple; OFF=500.69,9432.76,-19909.81
        cunit l4_temple || exit 1
        for j in 50 55; do
            CAMDUMP= VARS="$VARS,yaw_bits:p:38,fadf90_bits:a:FADF90,fadf94_bits:a:FADF94" \
                creplay "$OU_CHART_DIR/runjump/t1204_run_jump_j$j.case.json" t1204_run_jump_j${j}_stick
        done ;;
    temple2)
        # ChartA (canonical replay drives the move axes from var_stick +0x00 / +0x04): stick takes of more t1204 cases
        UNIT=l4_temple; OFF=500.69,9432.76,-19909.81
        cunit l4_temple || exit 1
        for n in t1204_stick_jump t1204_run_jump_j80 t1204_run_jump_j85 t1204_run_jump_j90; do
            CAMDUMP= creplay "${CASEDIR:-$OU_CHART_DIR}/runjump/$n.case.json" ${n}_stick runjump
        done ;;
    sjo)
        UNIT=l3_main_bedroom; OFF=9622,333,-2977
        cunit l3_main_bedroom || exit 1
        S=1137.7,-3997,-2733,0; i=0   # warm-up: the first jumps after a new game miss ledge 973
        until SPAWN=$S HOLD=60 CAMFOLLOW=0,-1,1264,300 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
            i=$((i+1)); [ $i -gt 20 ] && { echo "ledge 973 never caught" >&2; exit 1; }
            SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null
        CAMDUMP= creplay "${CASEDIR:-$OU_CHART_DIR}/jump/stick_jump_open.case.json" stick_jump_open_stick jump ;;
    esac
done
