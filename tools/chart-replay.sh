#!/bin/bash
# Canonical replay of the Wine chart corpus (W/chart/wine, README.md) with the chart driving Lara (--traversal chart),
# the one runner every agent quotes corpus totals from (tools/chart-compare.py for the table).
#
#   tools/chart-replay.sh OUT_DIR CASE...          CASE = dir/name under W/chart/wine (e.g. ledge/hang_grab)
#   env: BIN (default build-ChartDrive/ou-game of this checkout), SOCK (kwin socket name, default ou-replay-$USER-$$),
#        SHADOW=1 (--chart-shadow instead of --traversal chart), MEMMAX (default 8G)
#
# Per case: the case JSON from W/chart/wine_startpos, else wine_settle (120 more settle ticks), else wine; spawn at
# start.ours whenever the case records it (idle starts too), else spawn.ours; a hold case (start.state not an idle
# 35 / 39 / 40) also gets
# --chart-state STATE@(hold + settle ticks)@start.ours@t30@spawn.ours (t30: Wine's elapsed frames in that state at case
# tick 0, the trace's t30 column; spawn.ours: the spawn hold's pose, from which a free-climb seed runs the falling grab
# search (profile 45) that tru.exe runs once on the hold's first frame, Shadow::grabAt; Wine cc_dice_right: 0x70F300's
# arc starts at spawn.ours' hang height, not start.ours'); the input script is the case JSON itself (its
# keys, the settle shift and camera_follow, which
# pins the stick's world yaw, InputScript::cameraYaw); frames = hold + settle + ticks. Runs headless in a private
# kwin_wayland --virtual under systemd-run (MemoryMax, KillMode=control-group), never on the user's desktop.
# Until the --chart-state tick drive mode pins her at start.ours; before a non-hold start state it runs no markup search
# and keeps the attach unchanged (docs/re/PLAYER_CHART.md, --chart-state; harness only, tru never stood pinned there).
# A case JSON with "no_enemies": true (set only for a take whose Wine trace shows no enemy contact; the case cites why)
# also gets --no-enemies (enemy AI frozen): our enemies would attack where tru's did not, and the hand-off to the legacy
# hit reaction restarts the chart. Never on by default.
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
    ${OU_CHART_CHANGES:+--setenv=OU_CHART_CHANGES=1} \
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
  # the Wine take the case JSON came from (a settle take has its own trace beside it), else the original take
  TSV=$(dirname "$J")/$b.tsv; [ -f "$TSV" ] || TSV=$W/chart/wine/$c.tsv
  # the take's recorded input (tools/chart-stick-record.py): the stick record (var_stick) replaces the mask's direction
  # bits, var_inms gives the filter's per-frame dt (frame_dt), var_cam38 the camera heading the stick is read against
  # per tick (camera_yaw)
  python3 "$HERE/tools/chart-stick-record.py" "$J" "$TSV" "$OUT/$b.case.json" 2>/dev/null && J=$OUT/$b.case.json
  args=$(python3 - "$J" "$TSV" <<'PY'
import csv, json, sys
d = json.load(open(sys.argv[1])); s = d['spawn']; st = d.get('start', {})
state = st.get('state', 40); hold = state not in (35, 39, 40) and 'ours' in st
lead = s['hold_ticks'] + s.get('settle_ticks', 0)
# Wine's pose at case tick 0 whenever the case records one (idle starts too: step941 starts 135 units from spawn.ours);
# the --chart-state seeding only for a hold
pose = st['ours'] if 'ours' in st else s['ours']
out = [d['unit'], ','.join(str(x) for x in pose), str(lead + d['ticks'])]
def t30():  # Wine's elapsed frames in the start state at case tick 0
    try:
        for r in csv.reader(open(sys.argv[2]), delimiter='\t'):
            if r and r[0] == '0':
                return next((x.split('=')[1] for x in r if x.startswith('t30=')), '0')
    except OSError:
        pass
    return '0'
if hold:
    out.append(f"{state}@{lead}@{','.join(str(x) for x in st['ours'])}@{t30()}@{','.join(str(x) for x in s['ours'][:4])}")
elif state in (35, 40) and 'state' in st:
    # an idle start past the 1-foot idle: the take's tick 0 is later than her birth / settle (Wine birth takes are
    # S40 from tick 0; the trace starts after the load), while our chart restarts in S39 at the placement and reaches
    # S40 only when ng_1foot_idle ends (T274, 30 frames): enter the recorded idle at the case start, no pose
    out.append(f"{state}@{lead}")
elif state == 39 and 'state' in st:
    # a take that starts in the 1-foot idle, part way through ng_1foot_idle (t1204 j50_inms t30 14: S40 at tick 2 by
    # T274 AnimationEnded; j50_stick t30 9.5: still S39 at tick 10, then T156 -> S37): enter it at the case start that
    # many frames in, no pose ('-' is no pose to --chart-state)
    out.append(f"39@{lead}@-@{t30()}")
print(' '.join(out))
PY
)
  read -r unit sp n cs <<< "$args"
  extra=(); [ -n "${cs:-}" ] && extra=(--chart-state "$cs")
  # tru.exe's recorded generator draws for the case (W/chart/wine/<family>/<case>.rng: one value per token, in call
  # order; replay only, ou-game --chart-rng)
  RNG=$W/chart/wine/$c.rng; [ -f "$RNG" ] && extra+=(--chart-rng "$RNG")
  python3 -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1])).get("no_enemies") is True else 1)' "$J" && extra+=(--no-enemies)
  timeout 300 "$BIN" --game-dir "$G" --level "pc-w\\$unit.drm" --spawn "$sp" --input-script "$J" "${mode[@]}" \
    --chart-trace "$OUT/$b.tsv" --frames "$n" --no-audio --no-movies --no-cinematics --renderer opengl \
    --width 320 --height 180 "${extra[@]}" > "$OUT/$b.log" 2>&1
  echo "$b exit $?" >> "$OUT/runlog.txt"
done
