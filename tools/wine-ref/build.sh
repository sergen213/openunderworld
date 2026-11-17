#!/bin/sh
# Build the Win32 helpers (src/*.c) into $OU_WREF_DIR/bin with mingw-w64 (i686: tru.exe is a 32-bit process).
set -e
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
mkdir -p "$WREF_BIN"
for c in "$WREF_SRC"/src/*.c; do
    n=$(basename "$c" .c)
    i686-w64-mingw32-gcc -O2 -static -o "$WREF_BIN/$n.exe" "$c" -lwinmm -luser32
    echo "built $WREF_BIN/$n.exe"
done
