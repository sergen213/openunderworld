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

# Headless default heap is 2G; tru.exe decompilation needs more. Keep <=4G: several JVMs + ASan builds OOM the host.
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-4G}"

[[ -x "$HEADLESS" ]] || { echo "analyzeHeadless not found at $HEADLESS (set GHIDRA_HOME)" >&2; exit 1; }

case "$OU_WORK" in
    "$(dirname "$TRU_EXE")"*) echo "refusing: OU_WORK is inside the game directory" >&2; exit 1 ;;
esac

program_exists() {
    [[ -f "$ANALYZED_STAMP" && -f "$PROJECT_DIR/$PROJECT_NAME.gpr" ]]
}

# Run analyzeHeadless; if another process holds the project lock, wait and retry (max ~5 min).
headless() {
    local out rc=0 try
    out="$(mktemp)"
    for try in $(seq 1 60); do
        rc=0
        "$HEADLESS" "$@" >"$out" 2>&1 || rc=$?
        grep -q "Unable to lock project" "$out" || break
        echo "[ghidra] project locked by another process; retry $try/60" >&2
        sleep 5
    done
    cat "$out"
    rm -f "$out"
    return $rc
}
