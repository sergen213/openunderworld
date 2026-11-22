#!/bin/sh
# Live terrain-face read in l4_traprooms (ChartB: the floor under pole 1336 that tru.exe lets her fall through).
# Level = the g_streamTracker (0xDBAB40, 0xD8-byte records: +0 id, +4 state byte (2 loaded), +8 Level*, +0xC name)
# record named l4_traprooms; Level+0 -> Terrain {+8 numTerrainGroups, +0xC TerrainGroup* (0x50 each)}; group 0 +0x30
# TerrainGroupExtra* (flags), +0x38 collision Mesh (0x58 header; faces at +0x34, 20 bytes each: v0 v1 v2, u8 flags +0xC,
# u8 mask +0xD, u16 client +0xE, u8 material +0x10) (docs/formats/COLLISION.md). Dumps faces 13583..13591 and
# 13809 / 13810 and a drop from tru (6958, 4474, -500). Writes $OU_WREF_DIR/terrainfaces.txt (raw hex + decode).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_terrainfaces.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l4_traprooms; OFF=-7342.10,25801.50,-7298.09
[ "$1" = noload ] || cunit $UNIT || exit 1
sleep 60   # let the unit stream in (cmeasure saw 3 instances right after the goto)
W="$WREF_SRC/w.sh"; O="$OU_WREF_DIR/terrainfaces.txt"; T="$OU_WREF_DIR/tf"; mkdir -p "$T"
rd() { "$W" wmem rd "$1" "$2" | tr -d '\r' > "$T/$3"; }
rd DBAB40 6912 tracker
LV=$(python3 - "$T/tracker" <<'PY'
import sys
b = bytes(int(x, 16) for x in open(sys.argv[1]).read().split())
for i in range(0, len(b) - 0xD8 + 1, 0xD8):
    r = b[i:i + 0xD8]
    if r[0xC:0xC + 12] == b'l4_traprooms' and r[4] == 2: print('%08x' % int.from_bytes(r[8:12], 'little')); break
PY
)
[ -n "$LV" ] || { echo "l4_traprooms not loaded" | tee "$O"; exit 1; }
p32() { python3 -c "import sys;b=bytes(int(x,16) for x in open('$T/$1').read().split());print('%08x'%int.from_bytes(b[$2:$2+4],'little'))"; }
rd "$LV" 16 level; TR=$(p32 level 0)
rd "$TR" 24 terrain; NG=$(p32 terrain 8); GP=$(p32 terrain 12)
rd "$GP" 80 group0; EX=$(p32 group0 0x30); MS=$(p32 group0 0x38)
[ "$EX" != 00000000 ] && rd "$EX" 16 extra0
rd "$MS" 88 mesh0; FC=$(p32 mesh0 0x34)
rd "$(printf %x $((0x$FC + 20 * 13583)))" 180 faces_13583
rd "$(printf %x $((0x$FC + 20 * 13809)))" 40 faces_13809
rd "$(printf %x $((0x$MS + 0)))" 88 mesh0
"$W" chart trace 60 11:5:9 > /dev/null; sleep 10
SPAWN=6958,4474,-500,3.14159 HOLD=2 "$W" chart trace 90 | tr -d '\r' > "$T/drop.tsv"
python3 - "$T" "$LV" "$TR" "$NG" "$GP" "$EX" "$MS" "$FC" <<'PY' | tee "$O"
import sys, struct, os
T, LV, TR, NG, GP, EX, MS, FC = sys.argv[1:]
h = lambda n: bytes(int(x, 16) for x in open(os.path.join(T, n)).read().split()) if os.path.exists(os.path.join(T, n)) else b''
print('Level %s Terrain %s numTerrainGroups %d groups %s; group0 extra %s mesh %s faces %s' % (LV, TR, int(NG, 16), GP, EX, MS, FC))
g = h('group0'); print('group0 raw', g.hex())
print('group0 globalOffset', struct.unpack_from('<3f', g, 0), 'id', struct.unpack_from('<i', g, 0x24)[0])
if h('extra0'): print('group0 extra flags %08x next %08x' % struct.unpack_from('<2I', h('extra0'), 0))
m = h('mesh0'); print('mesh0 raw', m.hex())
for name, first in (('faces_13583', 13583), ('faces_13809', 13809)):
    b = h(name)
    for k in range(len(b) // 20):
        v0, v1, v2, fl, mk, cl, mat = struct.unpack_from('<3IBBHB', b, 20 * k)
        print('face %d v %d %d %d flags %02x mask %02x client %04x material %d raw %s' % (first + k, v0, v1, v2, fl, mk, cl, mat, b[20*k:20*k+20].hex()))
rows = [l.split('\t') for l in open(os.path.join(T, 'drop.tsv')).read().split('\n')[1:] if l]
for r in rows[::10]: print('drop tick %s S%s tru z %s' % (r[0], r[1], [x for x in r if x.startswith('var_tpz=')][0][8:]))
PY
