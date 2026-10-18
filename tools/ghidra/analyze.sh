#!/usr/bin/env bash
# Headless Ghidra import + auto-analysis + exports for tru.exe.
#
# Usage: tools/ghidra/analyze.sh [--overwrite] [--decompile-all] [--exports-only]
#   --overwrite      re-import and re-analyze even if the program already exists
#   --decompile-all  additionally decompile every function into exports/decomp_all.c
#   --exports-only   skip import/analysis, only (re)run post-scripts (default if program exists)
#
# Post-scripts (in order): ApplyNames (names/tru_gog.csv), ExportRtti (also labels vftables),
# ExportFunctions, ExportStrings, ExportImports. Names/labels are saved into the project.
#
# Environment overrides:
#   GHIDRA_HOME   Ghidra install (default /opt/ghidra)
#   TRU_EXE       input binary   (default GOG install path below; opened read-only)
#   OU_WORK       work root      (default ../openunderworld-work next to the repo)
#   GHIDRA_HEADLESS_MAXMEM  JVM heap (default 4G)
#
# Everything written (project, logs, exports) lands under $OU_WORK/ghidra. The game
# directory is only read. Exports contain proprietary-derived data: never commit them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

OVERWRITE=0
DECOMPILE_ALL=0
EXPORTS_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --overwrite) OVERWRITE=1 ;;
        --decompile-all) DECOMPILE_ALL=1 ;;
        --exports-only) EXPORTS_ONLY=1 ;;
        -h|--help) sed -n '2,19p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

[[ -r "$TRU_EXE" ]] || { echo "cannot read $TRU_EXE" >&2; exit 1; }
mkdir -p "$PROJECT_DIR" "$EXPORT_DIR" "$LOG_DIR"

sha256sum "$TRU_EXE" | cut -d' ' -f1 > "$GHIDRA_WORK/input.sha256"
POST=(
    -postScript ApplyNames.java "$SCRIPT_DIR/names/tru_gog.csv"
    -postScript ExportRtti.java "$EXPORT_DIR" apply
    -postScript ExportFunctions.java "$EXPORT_DIR"
    -postScript ExportStrings.java "$EXPORT_DIR"
    -postScript ExportImports.java "$EXPORT_DIR"
)
if [[ $DECOMPILE_ALL -eq 1 ]]; then
    POST+=(-postScript DecompileAll.java "$EXPORT_DIR/decomp_all.c")
fi

COMMON=(
    "$PROJECT_DIR" "$PROJECT_NAME"
    -scriptPath "$SCRIPT_DIR/scripts"
    -log "$LOG_DIR/headless.log"
    -scriptlog "$LOG_DIR/scripts.log"
)

if [[ $OVERWRITE -eq 0 ]] && program_exists; then
    EXPORTS_ONLY=1
fi

if [[ $EXPORTS_ONLY -eq 1 ]]; then
    program_exists || { echo "no analyzed program in $PROJECT_DIR; run without --exports-only" >&2; exit 1; }
    echo "[analyze] program exists; running exports only (use --overwrite to re-analyze)"
    headless "${COMMON[@]}" -process "$PROGRAM_NAME" -noanalysis "${POST[@]}"
else
    echo "[analyze] importing + analyzing $TRU_EXE"
    headless "${COMMON[@]}" -import "$TRU_EXE" -overwrite \
        -processor "x86:LE:32:default" -cspec windows \
        -analysisTimeoutPerFile 7200 "${POST[@]}"
    touch "$ANALYZED_STAMP"
fi

echo "[analyze] exports in $EXPORT_DIR:"
ls -la "$EXPORT_DIR"
