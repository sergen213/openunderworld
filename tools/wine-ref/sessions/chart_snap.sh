#!/bin/sh
# Chapel hang snap (ChartDrive): in ch_hang_* the S533 -> S447 change (ticks 19..23) moves tru.exe's root 78.9 along
# ledge 1063 to the segment end (grip player+0xC98 0.898 -> 1.0). ch_hang_left_hold (clean before its carry-free window,
# README "Carry after SPAWN") replayed for 60 ticks with the call log on the candidates, two lists (CALLFN takes 8):
#   A: the named suspects 0x74ED30 0x741130 0x76D010 0x76D270 0x770ED0 and the S533 actions' functions 0x772CA0
#      (230 MarkUpAttach_SetAnimTarget preenter), 0x75E5C0 / 0x76DB40 (411 GrabHelperObject enter / update);
#   B: functions that store player+0xC98 (decomp_all.c): 0x707530 0x707730 0x707990 0x708F90 0x741CB0 0x74E5A0
#      0x74DCD0 0x752870;  C: the rest 0x72B2C0 0x733210 0x738050 0x74C110 0x7503B0.
# Per tick VARS player+0xC94 / +0xC98 / +0xCA0 / +0x1260..+0x1268. Writes pole-free corpus files
# ledge/ch_hang_snap_{a,b,c}.tsv (not cases: diagnostics).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_snap.sh [a b c]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
W="$WREF_SRC/w.sh"
UNIT=l3_chapel; OFF=3196.89,-2412.27,3634.58
r=0; until cunit l3_chapel; do r=$((r+1)); [ $r -ge 3 ] && exit 1; done
T="$OU_WREF_DIR/ch_hang_snap.case.json"
python3 - "$OU_CHART_DIR/ledge/ch_hang_left_hold.case.json" "$T" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c['ticks'] = 60; json.dump(c, open(sys.argv[2], 'w'), indent=1)
PY
export VARS=c94:d:C94:f,c98:d:C98:f,ca0:d:CA0,ca0f:d:CA0:f,t1260:d:1260:f,t1264:d:1264:f,t1268:d:1268:f,i20:p:20:f,i24:p:24:f,i28:p:28:f,ic0:p:C0:f,ic4:p:C4:f,ic8:p:C8:f,d6f0:d:6F0:f,d6f4:d:6F4:f,d6f8:d:6F8:f
for part in ${*:-a b c}; do
    case $part in
    a) L=74ED30,741130,76D010,76D270,770ED0,772CA0,75E5C0,76DB40 ;;
    b) L=707530,707730,707990,708F90,741CB0,74E5A0,74DCD0,752870 ;;
    c) L=72B2C0,733210,738050,74C110,7503B0 ;;
    # d: functions that take a pointer to player+0xC8C..+0xCA0 (lea; the store goes through it): 0x7296C0 0x74C8D0
    #    0x75D8D0 0x770BA0 0x772C00, and the remaining direct writers 0x737D60 0x753F80 (2026-11-22: a/b/c show the
    #    grip already 0.9999 after the frame of tick 19, only 0x76D010 / 0x76DB40 / 0x770ED0 of a-c running)
    d) L=7296C0,74C8D0,75D8D0,770BA0,772C00,737D60,753F80 ;;
    # e: bracket by order: each entry copies player+0xC94..+0xCA0 (:P+C94) on the chart driver (0x7857D0 state update,
    #    0x787570 / 0x7876E0 transition scans, 0x787E60) and 0x781D90 (calls 0x770ED0), 0x76D010, 0x770BA0
    e) L=781D90:L=6:P+C94,7857D0:L=9:P+C94,787570:L=8:P+C94,7876E0:L=6:P+C94,787E60:L=10:P+C94,76D010:P+C94,770BA0:P+C94 ;;
    # f: e showed the change inside the hang process 0x770ED0 after its 0x76D010 call (0x771210): its callees 0x708830,
    #    0x75D6C0 (ecx = player+0xCA0, args +0xC94 / +0xC98), 0x711120, 0x770BA0, bracketed (entry and return copies)
    f) L=708830:P+C94:RP+C94,75D6C0:P+C94:RP+C94,711120:P+C94:RP+C94,770BA0:P+C94:RP+C94 ;;
    # g: f found the change inside 0x708830 (from 0x771262; entry grip 0.898451, return 0.9999, +0xCA0 8636.47 ->
    #    8715.39 at tick 19 only): its conditional callee 0x708210 (param_1+0x48 / +0x47 flags; arg5 = param_1, copied
    #    from +0x44), bracketed; 0x708830 itself with its param_1 +0x44..+0x53 (the flags) at entry
    g) L=708210:P+C94:RP+C94:5+44,708830:1+44 ;;
    # h (ChartDrive): 0x708210's param_6 (708830's local_d0 = 0x6FE3E0's root delta: instance +0x20 - +0xC0 - player
    #    data +0x6F0, 4 floats) on entry, with the per-tick instance +0x20 / +0xC0 and player data +0x6F0 below
    h) L=708210:6:P+C94:RP+C94 ;;
    beam) # ChartB: beam/ch_beam_walk's hang (ticks 0..120, before its Jump) with player+0xED4 / +0xC98 / +0xC94 and the
        # wall-under-hang ray (RAY=1); the hang sets her position, so the post-SPAWN carry does not reach this window
        python3 - "$OU_CHART_DIR/beam/ch_beam_walk.case.json" "$OU_WREF_DIR/ch_beam_hang.case.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c['ticks'] = 120; json.dump(c, open(sys.argv[2], 'w'), indent=1)
PY
        RAY=1 VARS=ed4:d:ED4,ed4f:d:ED4:f,c94:d:C94,c98:d:C98:f,ca0f:d:CA0:f creplay "$OU_WREF_DIR/ch_beam_hang.case.json" ch_beam_walk_hang_ray beam
        rm -f "$OU_CHART_DIR/beam/ch_beam_walk_hang_ray.case.json"; continue ;;
    esac
    CALLS=1 CALLFN=$L creplay "$T" ch_hang_snap_$part ledge
    rm -f "$OU_CHART_DIR/ledge/ch_hang_snap_$part.case.json"
    CALLFN=$L "$W" chart callunhook
done
