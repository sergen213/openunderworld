#!/bin/sh
# Unpinned-camera takes (TraversalCam: tru.exe's own climbing camera). Replays of existing cases with NOCAM=1 (no
# CAMFOLLOW: tru's camera runs free, also during the reset) and CAMDUMP=1 (chart.c: the matrices 0x5E7850 / 0x5E7870
# return every tick). Each case.json gets `camera_at_key`: at the first tick with a key held (else tick 0) the camera
# position (cam38 row 3), her raw position, the horizontal yaw from the camera to her and the camera's -row 2 / row 1
# headings (degrees, tru frame = ours frame rotation), so the starting camera is known. Files: <family>/<case>_freecam.
# Usage (session dir exported, see chart_lib.sh): sessions/chart_freecam.sh [bedroom|chapel|ship|traprooms|temple|junglehub]...
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
export NOCAM=1 CAMDUMP=1
fc() { # FAMILY CASE [NEWNAME]: replay CASE as NEWNAME (default CASE_freecam), then note the camera at the first key
    # (CAMUNTIL set: at the tick the pin was released, var_campin's first 0, recorded as camera_released_tick)
    creplay "$OU_CHART_DIR/$1/$2.case.json" "${3:-$2_freecam}" "$1"
    python3 - "$OU_CHART_DIR/$1/${3:-$2_freecam}" <<'PY'
import json, math, sys
base = sys.argv[1]; rows = [l.split('\t') for l in open(base + '.tsv').read().split('\n')[1:] if l]
rel = next((r for r in rows if 'var_campin=0' in r), None)
pick = rel or next((r for r in rows if int(r[12], 16)), rows[0])
d = {x.split('=', 1)[0]: x.split('=', 1)[1] for x in pick[13:] if '=' in x}
m = [float(v) for v in d['var_cam38'].split(':', 1)[1].split(',')]
cam = m[12:15]; me = [float(d['var_tpx']), float(d['var_tpy']), float(d['var_tpz'])]
hd = lambda x, y: round(math.degrees(math.atan2(y, x)), 2)
note = {'tick': int(pick[0]), 'state': int(pick[1]), 'camera_pos_tru': [round(v, 2) for v in cam], 'lara_pos_tru': me,
        'yaw_camera_to_lara_deg': hd(me[0] - cam[0], me[1] - cam[1]), 'heading_minus_row2_deg': hd(-m[8], -m[9]),
        'heading_row1_deg': hd(m[4], m[5]), 'cam38': m}
c = json.load(open(base + '.case.json')); c['camera_at_key'] = note
if rel: c['camera_released_tick'] = int(rel[0]); c['notes'] = 'camera pinned (CAMFOLLOW) until the first ladder state S1066, then free (CAMUNTIL), CAMDUMP every tick; ' + c['notes']
else: c['notes'] = 'UNPINNED camera (NOCAM, CAMDUMP every tick); ' + c['notes']
json.dump(c, open(base + '.case.json', 'w'), indent=1)
print(base.split('/')[-1], 'camera at key', note['tick'], 'yaw cam->Lara', note['yaw_camera_to_lara_deg'])
PY
}
warm973() { S=1137.7,-3997,-2733,0; i=0   # the first jumps after a new game miss ledge 973
    until SPAWN=$S HOLD=60 "$WREF_SRC/w.sh" chart trace 120 20:10:14 | tr -d '\r' | awk -F'\t' '$2==535{f=1} END{exit !f}'; do
        i=$((i+1)); [ $i -gt 20 ] && { echo "ledge 973 never caught" >&2; return 1; }
        SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 60 11:5:9 > /dev/null; sleep 10; done
    SPAWN=$S HOLD=10 "$WREF_SRC/w.sh" chart trace 120 11:5:9 > /dev/null; }
for part in ${*:-bedroom chapel ship traprooms temple junglehub}; do
    case $part in
    bedroom) UNIT=l3_main_bedroom; OFF=9622,333,-2977; cunit $UNIT || exit 1; warm973 || exit 1
        for n in hang_grab hang_shimmy_left hang_shimmy_right hang_pullup; do fc ledge $n; done ;;
    chapel) UNIT=l3_chapel; OFF=3196.89,-2412.27,3634.58; cunit $UNIT || exit 1
        for n in ch_hang_land ch_hang_left_hold ch_hang_right_hold; do fc ledge $n; done ;;
    ship) UNIT=l1_ship_sinking_hall; OFF=34604.60,3795.87,12744.02; cunit $UNIT || exit 1
        for n in lad_a234_attach lad_a234_climb_up; do fc ladder $n; done ;;
    ladder) UNIT=l1_ship_sinking_hall; OFF=34604.60,3795.87,12744.02; cunit $UNIT || exit 1; sleep 60; cmeasure $UNIT
        # TraversalCam: pinned until S1066 (the first ladder state), then tru's camera free
        NOCAM= CAMUNTIL=1066 fc ladder lad_a234_climb_up lad_a234_climb_up_freecam
        NOCAM= CAMUNTIL=1066 fc ladder lad_a234_left_hold lad_a234_side_freecam ;;
    traprooms) UNIT=l4_traprooms; OFF=-7342.10,25801.50,-7298.09; cunit $UNIT || exit 1
        for n in tr_pole_grab tr_pole_hoh_right tr_pole_swing; do fc pole $n; done ;;
    temple) UNIT=l4_temple; OFF=500.69,9432.76,-19909.81; cunit $UNIT || exit 1
        fc pillar t1194_climb_up_t1 ;;
    junglehub) UNIT=l2_junglehub; OFF=8558.2,-19414.5,-18073.1; cunit $UNIT || exit 1
        fc chimney jh_stick_jump_wall ;;
    esac
done
