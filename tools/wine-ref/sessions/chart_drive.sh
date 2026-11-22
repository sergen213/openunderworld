#!/bin/sh
# Drive-mode probes (ChartDrive / Main): where tru.exe gets (1) the hang distance (her root 161 out from ledges 973 / 1155)
# and (2) the stick-jump crossfade drift (~170 units over S245's first 8 ticks). Replays with extra columns:
#   hang: pd +0xCA0 grab point P, +0xCC0 ledge normal n, +0xCE4 / +0xCE8 grab offsets, +0x1260 / +0x1270 the alignment
#   target, +0xC94 / +0xC98 the attached markup; the layer's alignment record L+0x00 / +0x10 / +0x20 / +0x24 (0x43E480);
#   entry calls of the class-610 target setters 0x74ED30 / 0x7571B0, 0x741130 (copies its target argument) and the
#   record writers 0x43E480 / 0x438DC0 (copy their position argument).
#   crossfade: channel 0 time / mode (+0x28, +0x60, +0x64), the layer's crossfade window L+0x70 / +0x74 and record r
#   (+0x50 outgoing, +0x60 incoming root velocity), calls of 0x441FB0 (crossfade start) and 0x43B870 (channel root
#   extraction: ECX channel, a1 time, a2 step).
# Files: ledge/hang_grab_drive, ledge/t1155_fall_catch_drive, jump/stick_jump_open_drive.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_drive.sh [bedroom|temple]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
f() { o=""; for x in "$@"; do o="$o,$x"; done; echo "${o#,}"; }
HANGVARS=$(f pd_c94:pd:C94 pd_c98:pd:C98:f pd_ca0:pd:CA0:f pd_ca4:pd:CA4:f pd_ca8:pd:CA8:f pd_cc0:pd:CC0:f pd_cc4:pd:CC4:f \
    pd_cc8:pd:CC8:f pd_ce4:pd:CE4:f pd_ce8:pd:CE8:f pd_1260:pd:1260:f pd_1264:pd:1264:f pd_1268:pd:1268:f \
    pd_1270:pd:1270:f pd_1274:pd:1274:f pd_1278:pd:1278:f pd_127c:pd:127C:f \
    L_0:L:0:f L_4:L:4:f L_8:L:8:f L_10:L:10:f L_14:L:14:f L_18:L:18:f L_1c:L:1C:f L_20:L:20:f L_24:L:24 \
    k_t:k:28:f k_60:k:60 k_64:k:64)
XFVARS=$(f k_t:k:28:f k_60:k:60 k_64:k:64 k_clip:k:24 L_70:L:70:f L_74:L:74:f L_7c:L:7C L_24:L:24 L_20:L:20:f \
    r_50:r:50:f r_54:r:54:f r_58:r:58:f r_60:r:60:f r_64:r:64:f r_68:r:68:f)
HANGFN=74ed30,7571b0,741130:2,43e480:2,438dc0:2
XFFN=441fb0,43b870
C="$OU_CHART_DIR"
for part in ${*:-bedroom temple}; do
    case $part in
    bedroom)
        UNIT=l3_main_bedroom; OFF=9622,333,-2977
        cunit l3_main_bedroom || exit 1
        S=1137.7,-3997,-2733,0; i=0   # warm-up: the first jumps after a new game miss ledge 973
        until SPAWN=$S HOLD=60 CAMFOLLOW=0,-1,1264,300 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
            i=$((i+1)); [ $i -gt 20 ] && { echo "ledge 973 never caught" >&2; exit 1; }
            SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null
        CALLS=1 CALLFN=$HANGFN VARS=$HANGVARS creplay "$C/ledge/hang_grab.case.json" hang_grab_drive
        CALLFN=$HANGFN "$WREF_SRC/w.sh" chart callunhook
        CALLS=1 CALLFN=$XFFN VARS=$XFVARS creplay "$C/jump/stick_jump_open.case.json" stick_jump_open_drive
        CALLFN=$XFFN "$WREF_SRC/w.sh" chart callunhook ;;
    temple)
        UNIT=l4_temple; OFF=500.69,9432.76,-19909.81
        cunit l4_temple || exit 1
        # t1155_fall_catch's own take settles 90 unrecorded ticks (it starts hanging); here the fall and catch are recorded.
        CALLS=1 CALLFN=$HANGFN VARS=$HANGVARS HOLD=60 SETTLE=0 ccase ledge t1155_fall_catch_drive 180,-2417.44,-17275.24,1.5708 200 1,0 \
            "t1155_fall_catch's spawn (600 over ledge 1155's hang root) without its settle: fall, air grab, hang; drive-mode columns"
        CALLFN=$HANGFN "$WREF_SRC/w.sh" chart callunhook ;;
    chapel)
        # ledge/ch_hang_land again (the first take started 180 over the ledge, 800 off it, with the old wrong offset):
        # pinned 600 over static ledge 1063's hang root at its middle (line (4740.6..5518.6, 3099.3, -6795.1) ours, hang
        # side -y, root 161 out and 498 under), facing the wall (+y), falls, air grab, hang.
        UNIT=l3_chapel; OFF=3196.89,-2412.27,3634.58
        cunit l3_chapel || exit 1
        # HOLD=2: with a longer pin the air grab already takes the ledge during the hold (the first take at HOLD=60
        # started in S533), so the fall would not be recorded.
        HOLD=2 SETTLE=0 ccase ledge ch_hang_land "$(tru 5129.6 2938.3 -6693.1 90)" 200 0,1 \
            "placed (2-tick pin) 600 over static ledge 1063's hang root (middle of the line, 161 out on the -y side, 498 under): falls, air grab, hang" ;;
    esac
done
