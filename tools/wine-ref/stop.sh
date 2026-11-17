#!/bin/sh
# Stop this harness's tru.exe, its wineserver (private prefix only) and its compositor scope.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
systemctl --user stop "$WREF_TRU_UNIT" 2>/dev/null; systemctl --user reset-failed "$WREF_TRU_UNIT" 2>/dev/null
wineserver -k 2>/dev/null; sleep 1
systemctl --user stop "$WREF_KWIN_UNIT.scope" 2>/dev/null
rm -f "$OU_WREF_DIR/display.env"
