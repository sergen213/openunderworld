#!/bin/sh
# w.sh <tool> args...: run one helper (bin/<tool>.exe) against the running tru.exe (found by its window class).
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix; wref_display || exit 1
t=$1; shift
exec wine "$WREF_BIN/$t.exe" "$@"
