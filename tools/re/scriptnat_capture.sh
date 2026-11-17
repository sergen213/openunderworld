#!/bin/sh
# Runtime capture of tru.exe's SCMD native table (docs/re/SCRIPT_NATIVES.md). Run as the child of a private compositor:
#   W=<scratch>; systemd-run --user --scope --unit=ou-scriptnat -q -p MemoryMax=14G -- env -u DISPLAY W=$W \
#     XDG_CONFIG_HOME=$W/cfg XDG_DATA_HOME=$W/data dbus-run-session -- kwin_wayland --virtual --xwayland --width 1280 \
#     --height 720 --socket ou-scriptnat --no-lockscreen --no-global-shortcuts --no-kactivities --exit-with-session \
#     tools/re/scriptnat_capture.sh
# Needs in $W: wineprefix/ (copy of openunderworld-work/wine), wkey.exe (openunderworld-work/scratch/climbcam/ouk),
# scriptnat_reader.exe (tools/re/scriptnat_reader.c). Game copy: $G (default openunderworld-work/wine-drive/game).
W=${W:-/mnt/1st4TB/Projects/openunderworld-work/re-baseline/ScriptNatives}
G=${G:-/mnt/1st4TB/Projects/openunderworld-work/wine-drive/game}
export WINEPREFIX=$W/wineprefix WINEDEBUG=-all DXVK_LOG_LEVEL=none WAYLAND_DISPLAY=
( cd "$G" && WINEDLLOVERRIDES="d3d9=n;dxwrapper=d" exec taskset -c 0-3 wine ./tru.exe ) > "$W/tru.log" 2>&1 &
sleep 25
wine "$W/wkey.exe" click "Tomb Raider: Underworld - Setup" Ok
sleep 30
k() { wine "$W/wkey.exe" key "$1" 250 > /dev/null; sleep "${2:-6}"; }
k 26; k 0D; k 25; k 0D 8; k 0D 90 # menu: new game, difficulty; then the prologue loads
wine "$W/scriptnat_reader.exe" code 08000004 09000008 10000010 65000000 > "$W/runtime_natives.txt" 2> "$W/reader.err"
wineserver -k
