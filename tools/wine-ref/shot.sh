#!/bin/sh
# shot.sh OUT.png: screenshot of tru.exe's window on the private compositor.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"; wref_display || exit 1
W=$(xdotool search --name '^Tomb Raider: Underworld$' | head -1)
import -window "$W" "$1"
