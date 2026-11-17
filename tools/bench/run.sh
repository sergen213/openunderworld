#!/bin/bash
# One command for the fixed benchmark (docs/audit/BASELINE_PERF_2026-11.md):
#   tools/bench/run.sh [--out DIR] [--ou-game PATH] [--runs N] [--only a,b] [--no-cold]
# Starts a private KWin (virtual 1920x1080 output, own Wayland socket, own D-Bus session) as a memory-capped user
# scope, runs tools/bench/bench.py inside it as KWin's session child and stops the scope afterwards. Arguments are
# passed to bench.py. Never touches the desktop session's KWin.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out="$repo/../openunderworld-work/bench/$(date +%Y%m%d-%H%M%S)"
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out=$2; shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
mkdir -p "$out"
out=$(cd "$out" && pwd)
unit="ou-bench-$$"
inner="$out/inner.sh"
{
  echo '#!/bin/bash'
  printf 'export OU_GAME_DIR=%q\n' "${OU_GAME_DIR:-/mnt/1st4TB/Tomb Raider Underworld}"
  printf 'exec python3 %q --out %q' "$here/bench.py" "$out"
  [ ${#args[@]} -eq 0 ] || printf ' %q' "${args[@]}"
  printf ' > %q 2>&1\n' "$out/bench.log"
} > "$inner"
chmod +x "$inner"
echo "bench: output $out (scope $unit)"
kwin_rc=0
systemd-run --user --scope --unit="$unit" -q -p MemoryMax=14G -- \
  env -u DISPLAY -u WAYLAND_DISPLAY dbus-run-session -- \
  kwin_wayland --virtual --socket "ou-bench-$$" --width 1920 --height 1080 --exit-with-session "$inner" \
  > "$out/kwin.log" 2>&1 || kwin_rc=$?
systemctl --user stop "$unit.scope" 2>/dev/null || true
[ "$kwin_rc" = 0 ] || echo "bench: kwin session exited with $kwin_rc (see $out/kwin.log)"
cat "$out/bench.log"
python3 "$here/bench.py" --compare "$out/bench.csv" > /dev/null  # exit status: 1 on a digest/tick mismatch or failed run
