#!/usr/bin/env bash
# Print references to addresses, symbols/imports, or strings in the analyzed tru.exe (TSV on stdout).
#
# Usage: tools/ghidra/xrefs.sh TARGET...
#   TARGET  hex address (0x5dfbe0), symbol or import name (QueryPerformanceCounter), or str:<substring>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

[[ $# -gt 0 ]] || { sed -n '2,5p' "${BASH_SOURCE[0]}"; exit 2; }
program_exists || { echo "no analyzed program in $PROJECT_DIR; run analyze.sh first" >&2; exit 1; }
OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT
headless "$PROJECT_DIR" "$PROJECT_NAME" -process "$PROGRAM_NAME" -noanalysis -readOnly \
    -scriptPath "$SCRIPT_DIR/scripts" -log "$LOG_DIR/xrefs.log" -scriptlog "$LOG_DIR/xrefs-scripts.log" \
    -postScript Xrefs.java "$OUT" "$@" >/dev/null 2>&1
cat "$OUT"
