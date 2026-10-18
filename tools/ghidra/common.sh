# Shared settings for tools/ghidra/*.sh (sourced, not executed).
# Expects SCRIPT_DIR and REPO_ROOT to be set by the caller.

GHIDRA_HOME="${GHIDRA_HOME:-/opt/ghidra}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
TRU_EXE="${TRU_EXE:-/mnt/1st4TB/Tomb Raider Underworld/tru.exe}"
OU_WORK="${OU_WORK:-$(cd "$REPO_ROOT/.." && pwd)/openunderworld-work}"

GHIDRA_WORK="$OU_WORK/ghidra"
PROJECT_DIR="$GHIDRA_WORK/project"
PROJECT_NAME="tru"
PROGRAM_NAME="$(basename "$TRU_EXE")"
EXPORT_DIR="$GHIDRA_WORK/exports"
LOG_DIR="$GHIDRA_WORK/logs"
ANALYZED_STAMP="$GHIDRA_WORK/.analyzed"

# Headless default heap is 2G; tru.exe analysis + decompilation benefits from more.
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"

[[ -x "$HEADLESS" ]] || { echo "analyzeHeadless not found at $HEADLESS (set GHIDRA_HOME)" >&2; exit 1; }

case "$OU_WORK" in
    "$(dirname "$TRU_EXE")"*) echo "refusing: OU_WORK is inside the game directory" >&2; exit 1 ;;
esac

program_exists() {
    [[ -f "$ANALYZED_STAMP" && -f "$PROJECT_DIR/$PROJECT_NAME.gpr" ]]
}
