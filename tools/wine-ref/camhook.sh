#!/bin/sh
# camhook.sh install | log
# install: detour CameraManager 0x5E8210 (the camera mode request: ecx = camera, arg1 = mode) into a code cave that
#   records (mode, caller) of every request whose mode differs from the current one (+0x58) in a 64-entry ring
#   (cave +0x100 = index, cave +0x200 = 8-byte entries), then runs the two overwritten instructions
#   (push esi; mov esi,[esp+8]) and jumps back to 0x5E8215. The cave address is kept in $OU_WREF_DIR/camcave.txt.
# log: print the ring as mode@caller, oldest slot first (the ring wraps after 64 requests).
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
W="$WREF_SRC/w.sh"
case $1 in
install)
    C=$($W wmem alloc | tr -d '\r\n'); echo "cave $C"; echo "$C" > "$OU_WREF_DIR/camcave.txt"
    python3 - "$C" "$W" <<'P'
import struct, subprocess, sys
C = int(sys.argv[1], 16); W = sys.argv[2]; IDX = C + 0x100; BUF = C + 0x200
b = bytes([0x60, 0x8B, 0x44, 0x24, 0x24, 0x8B, 0x54, 0x24, 0x20, 0x3B, 0x41, 0x58, 0x74, 0x1d, 0x8B, 0x1D]) + struct.pack('<I', IDX)
b += bytes([0x83, 0xE3, 0x3F, 0x89, 0x04, 0xDD]) + struct.pack('<I', BUF) + bytes([0x89, 0x14, 0xDD]) + struct.pack('<I', BUF + 4)
b += bytes([0xFF, 0x05]) + struct.pack('<I', IDX) + bytes([0x61, 0x56, 0x8B, 0x74, 0x24, 0x08, 0xE9])
b += struct.pack('<i', 0x5E8215 - (C + len(b) + 4))
def wb(a, d):
    for i in range(0, len(d), 60):
        subprocess.run([W, 'wmem', 'wb', '%X' % (a + i), d[i:i + 60].hex()], capture_output=True)
wb(C, b); wb(0x5E8210, bytes([0xE9]) + struct.pack('<i', C - (0x5E8210 + 5)))
P
    $W wmem rd 5E8210 5;;
log)
    C=$(cat "$OU_WREF_DIR/camcave.txt")
    $W wmem rd "$(printf %X $((0x$C + 0x200)))" 512 | python3 -c '
import sys, struct
b = bytes.fromhex("".join(sys.stdin.read().split()))
print(" ".join("%d@%x" % struct.unpack("<iI", b[i*8:i*8+8]) for i in range(64) if struct.unpack("<iI", b[i*8:i*8+8])[1]))';;
*) echo "usage: camhook.sh install|log" >&2; exit 2;;
esac
