#!/bin/sh
# Hang-catch probe (ChartB / Main): replays of the l3_chapel `ledge/ch_hang_*` catches and of l3_main_bedroom
# `ledge/hang_shimmy_right` with the attach fields per tick and tru.exe's wall-under-hang ray. Columns: VARS
# pd_ed4 / pd_ed8 / pd_edc / pd_c98 = *(0xFADB58)+0xED4 / +0xED8 / +0xEDC / +0xC98 and p_ed4 / p_ed8 / p_edc / p_c98 the
# same offsets on the instance *(0xE7F094) (which base the "attach surface class" lives on is ChartB's call; :f marks
# the +0xC98 float), and RAY=1 (chart.c: 0x744E90 entry args + matrix, its 0x4935F0 result struct with flags 0x2C19E7).
# The chapel replays use each case's own tru-frame spawn and keys (creplay), so they repeat the original catches.
# Files: ledge/<case>_probe. Usage (session dir exported, see chart_lib.sh): sessions/chart_hangprobe.sh [chapel|bedroom]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
export RAY=1 VARS=pd_ed4:pd:ED4,pd_ed8:pd:ED8,pd_edc:pd:EDC,pd_c98:pd:C98:f,p_ed4:p:ED4,p_ed8:p:ED8,p_edc:p:EDC,p_c98:p:C98:f
C="$OU_CHART_DIR/ledge"
for part in ${*:-chapel bedroom}; do
    case $part in
    chapel)
        UNIT=l3_chapel; OFF=3817.47,-3091.84,3143.17   # the offset the ch_hang_* cases were recorded with
        cunit l3_chapel || exit 1
        for n in land walk walk_right_fall walk_left_fall left_hold right_hold jump crouch; do
            creplay "$C/ch_hang_$n.case.json" ch_hang_${n}_probe
        done ;;
    bedroom)
        UNIT=l3_main_bedroom; OFF=9622,333,-2977
        cunit l3_main_bedroom || exit 1
        S=1137.7,-3997,-2733,0; i=0
        until RAY=0 SPAWN=$S HOLD=60 CAMFOLLOW=0,-1,1264,300 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | tee "$OU_WREF_DIR/probe_$i.tsv" | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
            i=$((i+1)); [ $i -gt 20 ] && { echo "ledge 973 never caught" >&2; exit 1; }
            SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null
        creplay "$C/hang_shimmy_right.case.json" hang_shimmy_right_probe
        creplay "$C/hang_grab.case.json" hang_grab_probe ;;
    esac
done
"$WREF_SRC/w.sh" chart rayunhook
