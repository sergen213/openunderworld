#!/bin/bash
# Canonical replay of the Wine chart corpus (W/chart/wine, README.md) with the chart driving Lara (--traversal chart),
# the one runner every agent quotes corpus totals from (tools/chart-compare.py for the table).
#
#   tools/chart-replay.sh OUT_DIR CASE...          CASE = dir/name under W/chart/wine (e.g. ledge/hang_grab)
#   env: BIN (default build-ChartDrive/ou-game of this checkout), SOCK (kwin socket name, default ou-replay-$USER-$$),
#        SHADOW=1 (--chart-shadow instead of --traversal chart), MEMMAX (default 8G)
#
# Per case: the case JSON from W/chart/wine_startpos, else wine_settle (120 more settle ticks), else wine; spawn at
# start.ours when the case begins in a hold (start.state not an idle 35 / 39 / 40), else spawn.ours; a hold case gets
# --chart-state STATE@(hold + settle ticks)@start.ours@t30 (Wine's elapsed frames in that state at case tick 0, the
# trace's t30 column); the input script is the case JSON itself (its keys, the settle shift and camera_follow, which
# pins the stick's world yaw, InputScript::cameraYaw); frames = hold + settle + ticks. Runs headless in a private
# kwin_wayland --virtual under systemd-run (MemoryMax, KillMode=control-group), never on the user's desktop.
set -u
W=/mnt/1st4TB/Projects/openunderworld-work
G="/mnt/1st4TB/Tomb Raider Underworld"
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${BIN:-$HERE/build-ChartDrive/ou-game}
if [ -z "${CHART_REPLAY_INNER:-}" ]; then
  [ $# -ge 2 ] || { sed -n 4,9p "$0"; exit 2; }
  OUT=$(realpath -m "$1"); shift
  mkdir -p "$OUT"
  exec systemd-run --user --wait --collect --pipe -q --same-dir -p MemoryMax="${MEMMAX:-8G}" -p KillMode=control-group \
    --setenv=CHART_REPLAY_INNER=1 --setenv=OUT="$OUT" --setenv=CASES="$*" --setenv=BIN="$BIN" --setenv=SHADOW="${SHADOW:-}" \
    --setenv=SDL_VIDEODRIVER=wayland \
    dbus-run-session kwin_wayland --virtual --xwayland --socket "${SOCK:-ou-replay-$(id -un)-$$}" --width 1280 --height 720 \
    --exit-with-session "$(realpath "$0")" > "$OUT/kwin.log" 2>&1
fi
export XDG_CONFIG_HOME="$OUT/xdg-config" XDG_DATA_HOME="$OUT/xdg-data"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
mode=(--traversal chart); [ -n "${SHADOW:-}" ] && mode=(--chart-shadow)
for c in $CASES; do
  J=$W/chart/wine_startpos/$c.case.json; [ -f "$J" ] || J=$W/chart/wine_settle/$c.case.json; [ -f "$J" ] || J=$W/chart/wine/$c.case.json
  b=$(basename "$c")
  # the case's own trace carries the stick record (var_stick): replay its pad instead of the mask's direction bits; the
  # record comes from the same take as the chosen case JSON (wine_settle's trace beside it, else the wine one)
  T=${J%.case.json}.tsv; [ -f "$T" ] || T=$W/chart/wine/$c.tsv
  python3 "$HERE/tools/chart-stick-record.py" "$J" "$T" "$OUT/$b.case.json" 2>/dev/null && J=$OUT/$b.case.json
  args=$(python3 - "$J" "$W/chart/wine/$c.tsv" <<'PY'
import csv, json, sys
d = json.load(open(sys.argv[1])); s = d['spawn']; st = d.get('start', {})
state = st.get('state', 40); hold = state not in (35, 39, 40) and 'ours' in st
lead = s['hold_ticks'] + s.get('settle_ticks', 0)
pose = st['ours'] if hold else s['ours']
out = [d['unit'], ','.join(str(x) for x in pose), str(lead + d['ticks'])]
if hold:
    t30 = '0'
    try:
        for r in csv.reader(open(sys.argv[2]), delimiter='\t'):
            if r and r[0] == '0':
                t30 = next((x.split('=')[1] for x in r if x.startswith('t30=')), '0')
                break
    except OSError:
        pass
    out.append(f"{state}@{lead}@{','.join(str(x) for x in st['ours'])}@{t30}")
print(' '.join(out))
PY
)
  read -r unit sp n cs <<< "$args"
  extra=(); [ -n "${cs:-}" ] && extra=(--chart-state "$cs")
  timeout 300 "$BIN" --game-dir "$G" --level "pc-w\\$unit.drm" --spawn "$sp" --input-script "$J" "${mode[@]}" \
    --chart-trace "$OUT/$b.tsv" --frames "$n" --no-audio --no-movies --no-cinematics --renderer opengl \
    --width 320 --height 180 "${extra[@]}" > "$OUT/$b.log" 2>&1
  echo "$b exit $?" >> "$OUT/runlog.txt"
done
