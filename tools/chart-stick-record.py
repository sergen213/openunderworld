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
# usage: tools/chart-stick-record.py CASE_JSON TRACE_TSV OUT_JSON; exit 1 (no file written) when the trace has neither.
import csv, json, struct, sys

case_json, trace, out = sys.argv[1:4]
stick, last, inms = [], None, {}
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
frame_dt = [[t, (inms[t] - inms[t - 1]) * 30 / 1000] for t in sorted(inms) if t - 1 in inms]
if not stick and not frame_dt:
    sys.exit(1)
d = json.load(open(case_json))
if stick:
    d['stick'] = stick
if frame_dt:
    d['frame_dt'] = frame_dt
json.dump(d, open(out, 'w'), indent=1)
