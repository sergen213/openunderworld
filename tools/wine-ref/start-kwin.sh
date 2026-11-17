#!/bin/sh
# Start a private nested compositor (kwin_wayland --virtual with Xwayland, 1280x720) in its own systemd scope.
# tru.exe and the helpers draw / send input there only; the user's desktop is never touched.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
mkdir -p "$OU_WREF_DIR/xdg/cfg" "$OU_WREF_DIR/xdg/data"; rm -f "$OU_WREF_DIR/display.env"
systemd-run --user --scope --unit="$WREF_KWIN_UNIT" -q -p MemoryMax=4G -- \
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_CONFIG_HOME="$OU_WREF_DIR/xdg/cfg" XDG_DATA_HOME="$OU_WREF_DIR/xdg/data" \
    dbus-run-session -- kwin_wayland --virtual --xwayland --width 1280 --height 720 --scale 1 \
    --socket "ou-$OU_WREF_NAME" --no-lockscreen --no-global-shortcuts --no-kactivities \
    --exit-with-session "$WREF_SRC/kwin-child.sh" > "$OU_WREF_DIR/kwin.log" 2>&1 &
i=0; while [ ! -s "$OU_WREF_DIR/display.env" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i+1)); done
[ -s "$OU_WREF_DIR/display.env" ] || { echo "compositor did not start (see $OU_WREF_DIR/kwin.log)" >&2; exit 1; }
cat "$OU_WREF_DIR/display.env"
