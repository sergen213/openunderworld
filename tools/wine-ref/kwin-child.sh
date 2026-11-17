#!/bin/sh
# Session child of the private compositor: publish its Xwayland DISPLAY, then keep the session alive.
echo "DISPLAY=$DISPLAY" > "$OU_WREF_DIR/display.env"
exec sleep infinity
