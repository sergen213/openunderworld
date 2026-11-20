#!/bin/sh
# Stop this harness's tru.exe, its wineserver (private prefix only) and its compositor service.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
systemctl --user stop "$WREF_TRU_UNIT" 2>/dev/null; systemctl --user reset-failed "$WREF_TRU_UNIT" 2>/dev/null
wineserver -k 2>/dev/null; sleep 1
# a helper's unix-side launcher (`start.exe /exec $WREF_BIN/...`) can outlive the wineserver: match this bin dir only
pkill -f "start.exe /exec $WREF_BIN/" 2>/dev/null
systemctl --user stop "$WREF_KWIN_UNIT.service" 2>/dev/null   # one unit per call: a missing unit fails the whole call
systemctl --user stop "$WREF_KWIN_UNIT.scope" 2>/dev/null
rm -f "$OU_WREF_DIR/display.env"
