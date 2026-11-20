#!/bin/sh
# Start a private nested compositor (kwin_wayland --virtual with Xwayland, 1280x720) as a transient systemd service.
# tru.exe and the helpers draw / send input there only; the user's desktop is never touched. A service (not a scope)
# with KillMode=control-group: when it stops, systemd also kills what the private D-Bus session activated
# (xdg-desktop-portal*, ksecretd, ...), which a scope left running forever.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
mkdir -p "$OU_WREF_DIR/xdg/cfg" "$OU_WREF_DIR/xdg/data"; rm -f "$OU_WREF_DIR/display.env"
systemctl --user stop "$WREF_KWIN_UNIT.service" 2>/dev/null; systemctl --user reset-failed "$WREF_KWIN_UNIT.service" 2>/dev/null
systemd-run --user --unit="$WREF_KWIN_UNIT" --collect -q -p MemoryMax=4G -p KillMode=control-group \
    -p StandardOutput="file:$OU_WREF_DIR/kwin.log" -p StandardError="file:$OU_WREF_DIR/kwin.log" \
    -E OU_WREF_DIR="$OU_WREF_DIR" -E PATH="$PATH" -E XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" -- \
    env -u DISPLAY -u WAYLAND_DISPLAY XDG_CONFIG_HOME="$OU_WREF_DIR/xdg/cfg" XDG_DATA_HOME="$OU_WREF_DIR/xdg/data" \
    dbus-run-session -- kwin_wayland --virtual --xwayland --width 1280 --height 720 --scale 1 \
    --socket "ou-$OU_WREF_NAME" --no-lockscreen --no-global-shortcuts --no-kactivities \
    --exit-with-session "$WREF_SRC/kwin-child.sh"
i=0; while [ ! -s "$OU_WREF_DIR/display.env" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i+1)); done
[ -s "$OU_WREF_DIR/display.env" ] || { echo "compositor did not start (see $OU_WREF_DIR/kwin.log)" >&2; exit 1; }
cat "$OU_WREF_DIR/display.env"
