#!/usr/bin/env python3
# Adds the recorded stick to a Wine corpus case: reads the case's trace (W/chart/wine/CASE.tsv) and, when its rows carry
# var_stick (the 0xFADFB0 +0xB0 input record as hex, WineChartTrace STICKDUMP), writes the case JSON with
# "stick": [[tick, x, y], ...] (the record's +0x00 / +0x04 pad: x right, y down, one entry per change) to OUT_JSON.
# ou-game's --input-script then drives the move axes from that record instead of the mask's direction bits, so the
# replay sees the stick on the tick tru.exe's chart did (the arrow keys' axis can reach it a frame after the key).
# usage: tools/chart-stick-record.py CASE_JSON TRACE_TSV OUT_JSON; exit 1 (no file written) when the trace has no record.
import csv, json, struct, sys

case_json, trace, out = sys.argv[1:4]
stick, last = [], None
for r in csv.reader(open(trace), delimiter='\t'):
    if not r or not r[0].isdigit():
        continue
    v = next((x.split('=', 1)[1] for x in r[13:] if x.startswith('var_stick=')), None)
    if v is None or ':' not in v:
        continue
    x, y = struct.unpack_from('<2f', bytes.fromhex(v.split(':', 1)[1]), 0)
    if (x, y) != last:
        stick.append([int(r[0]), x, y])
        last = (x, y)
if not stick:
    sys.exit(1)
d = json.load(open(case_json))
d['stick'] = stick
json.dump(d, open(out, 'w'), indent=1)
