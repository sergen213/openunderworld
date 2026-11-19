#!/usr/bin/env python3
# Pixel-shader constants, bound textures and bool mask of one material's model draws in a running tru.exe (GOG 1.1).
#   cave A detours 0x526920 (the state manager's SetPixelShaderConstantF wrapper: thiscall reg, data, count; every PS
#   constant write goes through it) and mirrors each write into MIR (256 x vec4).
#   cave B detours 0x50EFED (0x50EE60's DrawIndexedPrimitive; esi = the draw record, ebp = the state manager) and logs
#   the draw's material blob ([[esi+0x20]]+4) into a 1024-entry ring. For the blob in BASE it counts the draw and copies
#   the mirror's RANGES, the state manager's 20 bound textures (+0x28) and its PS bool mask (+0x4B0) into one table
#   entry (the last such draw wins).
# The detours are written with the game's threads suspended (wmem wbs): patching 0x526920 while it runs crashes it.
# Usage (OU_WREF_DIR / OU_WREF_NAME as for w.sh; the cave address is kept in $OU_WREF_DIR/regcap.txt):
#   regcap.py install                 caves + detours (once per tru.exe process; BASE = 0 records nothing)
#   regcap.py find P0 P1 ...          BASE = the logged blob whose MaterialParams (blob +0x58 count, +0x5C ptr: c100..)
#                                     start with the floats P0 P1 ... (e.g. co_yacht material 6179: 0.67451 0.658824)
#   regcap.py read                    print the table entry, the ring's most drawn blobs and the mirror's c140-c149
import os, struct, subprocess, sys
from collections import Counter
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(SRC, 'w.sh')
STATE = os.path.join(os.environ.get('OU_WREF_DIR', os.path.join(os.environ.get('OU_WORK', '.'), 'wine-ref')), 'regcap.txt')
RANGES = [(15, 1), (70, 7), (96, 1), (100, 5), (140, 10), (176, 2), (178, 30), (208, 5)]
SZ = 1152
TEXO = 16 + 16 * sum(n for _, n in RANGES)
def w(*a): return subprocess.run([W, *a], capture_output=True, text=True).stdout
def wb(a, d):
    for i in range(0, len(d), 60): w('wmem', 'wb', '%X' % (a + i), d[i:i + 60].hex())
def rd(a, n):
    out = b''
    while n > 0:
        k = min(n, 4096); out += bytes.fromhex(''.join(w('wmem', 'rd', '%X' % a, str(k)).split())); a += k; n -= k
    return out
I = lambda v: struct.pack('<I', v & 0xFFFFFFFF)
def jmp(src, dst): return b'\xE9' + struct.pack('<i', dst - (src + 5))
def lay(C): return dict(A=C, B=C + 0x100, BASE=C + 0x300, RINGN=C + 0x400, RING=C + 0x410, MIR=C + 0x2000, TAB=C + 0x3000)
def cave(): return lay(int(open(STATE).read(), 16))
def install():
    C = int(w('wmem', 'alloc', '0xC000').strip(), 16); L = lay(C)
    a = bytearray(b'\x60\x8B\x44\x24\x24\x8B\x74\x24\x28\x8B\x4C\x24\x2C\x3D' + I(0x100) + b'\x73\x00'); j1 = len(a) - 1
    a += b'\x8D\x14\x08\x81\xFA' + I(0x100) + b'\x77\x00'; j2 = len(a) - 1      # reg < 256 and reg + count <= 256
    a += b'\xC1\xE0\x04\x8D\xB8' + I(L['MIR']) + b'\xC1\xE1\x02\xF3\xA5'        # copy count vec4s to MIR + reg * 16
    a[j1] = len(a) - (j1 + 1); a[j2] = len(a) - (j2 + 1)
    a += b'\x61\x8B\x54\x24\x0C\x8B\x41\x0C'                                    # popad; the 7 displaced bytes
    a += jmp(L['A'] + len(a), 0x526927)
    b = bytearray(b'\x60\x8B\x46\x20\x8B\x00\x8B\x40\x04')                     # eax = material blob
    b += b'\x8B\x15' + I(L['RINGN']) + b'\x81\xE2' + I(0x3FF) + b'\x89\x04\x95' + I(L['RING']) + b'\xFF\x05' + I(L['RINGN'])
    b += b'\x2B\x05' + I(L['BASE']) + b'\x3D' + I(1) + b'\x0F\x83' + I(0); j = len(b) - 4  # jae near: the copy exceeds rel8
    b += b'\x69\xC0' + I(SZ) + b'\x05' + I(L['TAB']) + b'\xFF\x00\x8D\x78\x10'
    for reg, n in RANGES: b += b'\xBE' + I(L['MIR'] + 16 * reg) + b'\xB9' + I(4 * n) + b'\xF3\xA5'
    b += b'\x8D\x75\x28\xB9' + I(20) + b'\xF3\xA5' + b'\x8B\x85' + I(0x4B0) + b'\xAB'  # bound textures, bool mask
    b[j:j + 4] = I(len(b) - (j + 4))
    b += b'\x61\x8B\x8B\x48\x01\x00\x00'                                        # popad; mov ecx,[ebx+0x148]
    b += jmp(L['B'] + len(b), 0x50EFF3)
    wb(L['BASE'], I(0)); wb(L['A'], bytes(a)); wb(L['B'], bytes(b))
    print(w('wmem', 'wbs', '526920', (jmp(0x526920, L['A']) + b'\x90\x90').hex()).strip())
    print(w('wmem', 'wbs', '50EFED', (jmp(0x50EFED, L['B']) + b'\x90').hex()).strip())
    open(STATE, 'w').write('%X' % C); print('cave %X' % C)
def find(prefix):
    L = cave(); seen = set()
    for _ in range(3): seen |= set(struct.unpack('<1024I', rd(L['RING'], 4096)))
    for blob in seen:
        h = rd(blob, 0x60)
        if len(h) < 0x60: continue
        n, p = struct.unpack_from('<II', h, 0x58)
        if not (0 < n < 64) or 4 * n < len(prefix): continue
        v = struct.unpack('<%df' % (4 * n), rd(p, 16 * n))
        if all(abs(x - y) < 1e-4 for x, y in zip(v, prefix)):
            wb(L['BASE'], I(blob)); print('blob %08X' % blob); return blob
    print('not found'); return 0
def read():
    L = cave(); base = struct.unpack('<I', rd(L['BASE'], 4))[0]
    n = struct.unpack('<I', rd(L['RINGN'], 4))[0]
    print('draws logged', n, 'top blobs', ['%08X:%d' % kv for kv in Counter(struct.unpack('<1024I', rd(L['RING'], 4096))).most_common(8)])
    e = rd(L['TAB'], SZ); cnt = struct.unpack_from('<I', e)[0]
    print('== blob %08X draws %d' % (base, cnt))
    o = 16
    for reg, k in RANGES:
        for r in range(k):
            print('  c%-3d %12.5f %12.5f %12.5f %12.5f' % ((reg + r,) + struct.unpack_from('<4f', e, o))); o += 16
    print('  textures', ' '.join('s%d=%08X' % (k, struct.unpack_from('<I', e, TEXO + 4 * k)[0]) for k in range(20)))
    print('  bools %08X' % struct.unpack_from('<I', e, TEXO + 80)[0])
    mir = rd(L['MIR'], 4096)
    for r in range(140, 150): print('  mirror c%d' % r, struct.unpack_from('<4f', mir, 16 * r))
if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'install': install()
    elif cmd == 'find': find([float(x) for x in sys.argv[2:]])
    elif cmd == 'read': read()
    else: sys.exit(__doc__ or 'usage: regcap.py install | find P0 P1 ... | read')
