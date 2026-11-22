#!/usr/bin/env python3
# Adds tru.exe's recorded input to a Wine corpus case from its trace (W/chart/wine*/CASE.tsv) and writes the case JSON
# to OUT_JSON:
#  - "stick": [[tick, x, y], ...] from var_stick (the 0xFADFB0 +0xB0 input record as hex, WineChartTrace STICKDUMP): its
#    +0x00 / +0x04 pad (x right, y down), one entry per change. ou-game drives the move axes from it (already the
#    output of the move stick filter 0x483520, which the replay then skips) instead of the mask's direction bits.
#  - "frame_dt": [[tick, dt], ...] from var_inms (InputSystem 0xA02B68 +0x1C, the last TIMER_GetTimeMilliseconds of
#    the input update 0x483520, hex): the dt in 30 Hz frames that update used, (inms[t] - inms[t-1]) x 30 / 1000, for
#    the frame of row t. Row t's update is the one row t's stick record reads (WineChartTrace _inms takes: the key at
#    tick 10 lands on row 10 when row 10's gap is 95 ms, 0.4 x 2.85 >= 1, and on row 11 when it is 70..72 ms).
#  - "camera_yaw": [[tick, yaw], ...] from var_cam38 (the camera's 4x4 world matrix, WineChartTrace CAMDUMP): yaw =
#    atan2 of its forward row 2 (x, y) in our frame (tru's stick heading B+0x5C is that + 90 deg; ChartB checked
#    h759_rng_birth t86 / t150 against var_a3b4 within 0.002 rad), one entry per change. The replay pins the stick's
#    world yaw to it per tick instead of the case's fixed camera_follow, as tru's free camera turns during a take.
#  - "camera_rows": [[tick, rx, ry, rz, dx, dy, dz, lx, ly, lz], ...] from the same matrix's rows 0 / 1 / 2 (g_camera
#    +0x10 right / down / look, ChartB: ch_hang_left_hold_stick), one entry per change: the replay pins the chart's
#    camera axes to them, which the stick class (0x6F89A0 -> 0x195) reads.
# usage: tools/chart-stick-record.py CASE_JSON TRACE_TSV OUT_JSON; exit 1 (no file written) when the trace has none.
import csv, json, math, struct, sys

case_json, trace, out = sys.argv[1:4]
stick, last, inms, cam_yaw, last_yaw, cam_rows, last_rows = [], None, {}, [], None, [], None
for r in csv.reader(open(trace), delimiter='\t'):
    if not r or not r[0].isdigit():
        continue
    t = int(r[0])
    cols = {x.split('=', 1)[0]: x.split('=', 1)[1] for x in r[13:] if '=' in x}
    v = cols.get('var_stick')
    if v and ':' in v:
        x, y = struct.unpack_from('<2f', bytes.fromhex(v.split(':', 1)[1]), 0)
        if (x, y) != last:
            stick.append([t, x, y])
            last = (x, y)
    if 'var_inms' in cols:
        inms[t] = int(cols['var_inms'].split(':')[-1], 16)
    c = cols.get('var_cam38')
    if c and ':' in c:
        m = [float(f) for f in c.split(':', 1)[1].split(',')]
        if len(m) == 16 and (m[8] or m[9]):
            yaw = round(math.atan2(m[9], m[8]), 5)
            if yaw != last_yaw:
                cam_yaw.append([t, yaw])
                last_yaw = yaw
            rows = [round(m[i], 5) for i in (0, 1, 2, 4, 5, 6, 8, 9, 10)]
            if rows != last_rows:
                cam_rows.append([t] + rows)
                last_rows = rows
frame_dt = [[t, (inms[t] - inms[t - 1]) * 30 / 1000] for t in sorted(inms) if t - 1 in inms]
if not stick and not frame_dt and not cam_yaw:
    sys.exit(1)
d = json.load(open(case_json))
if stick:
    d['stick'] = stick
if frame_dt:
    d['frame_dt'] = frame_dt
if cam_yaw:
    d['camera_yaw'] = cam_yaw
if cam_rows:
    d['camera_rows'] = cam_rows
json.dump(d, open(out, 'w'), indent=1)
