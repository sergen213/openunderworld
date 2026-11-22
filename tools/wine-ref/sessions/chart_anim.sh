#!/bin/sh
# Chart traces with Lara's main anim layer and the anim call log (chart.c ANIM=1 CALLS=1; columns spec'd by ChartJ:
# why S245, pattern 680, 5 keys x 100, start 1, blend 4, lasts 15 ticks): the bedroom jumps (stick / standing / running
# jump at ledge 973, as chart_bedroom.sh) and the l2_junglehub stick jump onto column A (wall jump, as
# chart_l2_junglehub.sh). Files get the suffix `_anim`. Two sessions.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_anim.sh [bedroom|junglehub]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
export ANIM=1 CALLS=1 VARS=ed4:pd:ED4,edc:pd:EDC,ee8:pd:EE8,a80:pd:A80,a64:pd:A64   # player-data dwords ChartJ asked for
HOLD=${HOLD:-60}
for part in ${*:-bedroom junglehub}; do
    case $part in
    bedroom)
        UNIT=l3_main_bedroom; OFF=9622,333,-2977
        cunit l3_main_bedroom || exit 1
        # The first jumps after the new game found no ledge 973 (chart_bedroom.sh): probe with grabs until one catches.
        # (A fixed 150 s wait instead let the prologue move on: she was sliding / falling far from the spawn.)
        S=1137.7,-3997,-2733,0; S2=1137.7,-3497,-2733,0; C=0,-1; i=0
        until SPAWN=$S HOLD=$HOLD CAMFOLLOW=$C,1264,300 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
            i=$((i+1)); [ $i -gt 10 ] && { echo "ledge 973 never caught" >&2; exit 1; }
            SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null
        ccase jump stick_jump_open_anim $S2 160 $C "Up + Jump at tick 10 (stick jump) towards ledge 973 from ~637 short; anim layer + call log" 26:10:60 20:10:14
        ccase jump stand_jump_open_anim $S2 140 $C "standing jump ~637 short of ledge 973; anim layer + call log" 20:10:14
        ccase jump run_jump_bedroom_anim "1137.7,-2600,-2733,0" 220 $C "run 10..70, Jump at 40 towards the ledge 973 wall; anim layer + call log" 26:10:70 20:40:46
        ;;
    junglehub)
        UNIT=l2_junglehub; OFF=8558.2,-19414.5,-18073.1
        cunit l2_junglehub || exit 1
        FL=-3870.8,3839.5,-13642.1,-1.5708; CA=-1,0
        cground chimney jh_stick_jump_wall_anim $FL 200 $CA "Up 20..56 + Jump 23..27: stick jump into column A: wall attach, hold, detach; anim layer + call log" 26:20:56 20:23:27
        cground chimney jh_stick_jump_rebound_anim $FL 200 $CA "stick jump into column A, Jump 56..60: rebound; anim layer + call log" 26:20:56 20:23:27 20:56:60
        ;;
    esac
done
"$WREF_SRC/w.sh" chart callunhook
