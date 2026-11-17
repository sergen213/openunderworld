# Sourced by every wine-ref script. Override any variable in the environment before calling a script.
# OU_WREF_DIR   session directory: built helpers (bin/), the compositor's DISPLAY (display.env), logs, traces
# WINEPREFIX    a private Wine prefix with DXVK's d3d9.dll installed (see README "Prerequisites")
# OU_TRU_DIR    a writable copy of the game install (tru.exe's directory) with D3D9.dll renamed away
# OU_WREF_NAME  name used for the compositor socket and the systemd units (unique per concurrent harness)
# The caller sets WREF_SRC (this directory) before sourcing.
: "${OU_WORK:=/mnt/1st4TB/Projects/openunderworld-work}"
: "${OU_WREF_DIR:=$OU_WORK/wine-ref}"
: "${WINEPREFIX:=$OU_WREF_DIR/prefix}"
: "${OU_TRU_DIR:=$OU_WORK/wine-drive/game}"
: "${OU_WREF_NAME:=wineref}"
export OU_WORK OU_WREF_DIR WINEPREFIX OU_TRU_DIR OU_WREF_NAME WREF_SRC
export WINEDEBUG=-all
WREF_BIN=$OU_WREF_DIR/bin
WREF_KWIN_UNIT=ou-$OU_WREF_NAME-kwin
WREF_TRU_UNIT=ou-$OU_WREF_NAME-tru
wref_display() { # DISPLAY of the private compositor's Xwayland (written by kwin-child.sh)
    [ -f "$OU_WREF_DIR/display.env" ] || { echo "no compositor: run start-kwin.sh first" >&2; return 1; }
    . "$OU_WREF_DIR/display.env"; export DISPLAY; unset WAYLAND_DISPLAY
}
