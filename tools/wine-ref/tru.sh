#!/bin/sh
# Start tru.exe on the private compositor as the systemd unit $WREF_TRU_UNIT (DXVK d3d9, the GOG wrapper off).
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"; wref_display || exit 1
systemctl --user stop "$WREF_TRU_UNIT" 2>/dev/null; systemctl --user reset-failed "$WREF_TRU_UNIT" 2>/dev/null
systemd-run --user --unit="$WREF_TRU_UNIT" -q -p MemoryMax=8G --working-directory="$OU_TRU_DIR" \
    -E DISPLAY="$DISPLAY" -E WAYLAND_DISPLAY= -E WINEPREFIX="$WINEPREFIX" -E WINEDEBUG=-all -E DXVK_LOG_LEVEL=none \
    -E WINEDLLOVERRIDES="d3d9=n;dxwrapper=d" -E PATH="$PATH" -E XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    taskset -c "${OU_WREF_CPUS:-0-3}" wine ./tru.exe "$@"
