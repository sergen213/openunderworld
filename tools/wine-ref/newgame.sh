#!/bin/sh
# (Re)start tru.exe and start a new game (Tomb Raider difficulty) into the prologue, l3_main_bedroom.
# The key sequence is the GOG 1.1 front end at 1280x720: setup dialog Ok, then Down / Enter / Up / Enter / Enter.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
W="$WREF_SRC/w.sh"
systemctl --user stop "$WREF_TRU_UNIT" 2>/dev/null; wineserver -k 2>/dev/null; sleep 2
"$WREF_SRC/tru.sh"
# the setup dialog can take minutes on a loaded machine: press Ok once it is there
i=0; until $W wkey click "Tomb Raider: Underworld - Setup" Ok | grep -q clicked || [ $i -ge 60 ]; do sleep 5; i=$((i+1)); done
sleep 30
k() { $W wkey key $1 250 > /dev/null; sleep ${2:-6}; }
k 26; k 0D; k 25; k 0D 8; k 0D 90
$W wmem mask
