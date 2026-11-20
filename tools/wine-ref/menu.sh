#!/bin/sh
# menu.sh: drive tru.exe's front end and pause menu by input (SendInput keys through wkey.exe), to start a session from a
# tru.exe save instead of a `wmem goto` jump-in, and to write saves at chosen points for reuse.
#
#   menu.sh setup               make the prefix's Documents private (see below) and create the save directory
#   menu.sh install [--only] FILE...   copy .TRUSave files into the save directory (--only: remove the others first,
#                               keeping Autosave.TRUSave, so that FILE is LOAD GAME slot 1)
#   menu.sh boot                start tru.exe (tru.sh), press Ok in the setup dialog if it opens, answer "Create autosave
#                               file?" with YES when the prefix has no Autosave.TRUSave; returns at the main menu
#   menu.sh load [SLOT]         main menu -> LOAD GAME -> slot SLOT (1..4, default 1) -> YES; waits until the level runs
#   menu.sh resume              main menu -> RESUME GAME (the autosave = the last checkpoint); waits until the level runs
#   menu.sh save LABEL [SLOT]   in play: pause -> SAVE GAME -> slot SLOT (default: the first empty one = number of saves
#                               + 1, at most 4) -> Return; copies the new file to $OU_WREF_DIR/saves/LABEL.TRUSave
#   menu.sh sel                 what the menu shows selected: `ring N slot M` (below; - = none)
#   menu.sh shot NAME           screenshot to $OU_WREF_DIR/NAME.png
#
# Menu layout (GOG 1.1, 1280x720 window, captures 2026-11-20; docs/GAMEPLAY.md "Menus"): ring items 0 the centre (PLAY
# GAME / RESUME GAME), 1 START NEW GAME, 2 GAME OPTIONS, 3 LOAD GAME, 4 SAVE GAME, 5 AUDIO/VISUAL, 6 EXTRAS, 7 QUIT GAME;
# Left leaves the centre for the ring item selected last (SAVE GAME the first time), Up / Down step along the ring,
# Right / Escape go back to the centre. Return on LOAD / SAVE GAME opens the slot ring on the right (0 page up, 1..4 the
# slots of the page, 5 page down) with no slot selected, Right enters it at slot 2; Escape leaves it. Loading asks "If you choose to load ..." with
# NO selected (Left, Return). Without Autosave.TRUSave the front end first asks "Create autosave file?" (NO selected).
#
# Keys are not trusted to land: on a loaded machine the menu runs at a few frames a second and a press can be missed,
# so every step reads the selection back from the screen (`sel`, two screenshots 0.3 s apart merged by their brighter
# pixel: the selected item draws its icon white (255) on a black disc, the others stay at or under ~150 in a 40x40 box
# at its centre at 1280x720; the item whose slot ring is open (LOAD / SAVE GAME) is circled in white, radius ~44 px)
# and repeats the key until the selection is where it should be. The front end's own state comes from memory
# (`wmem state`: main state 10 / sub-state 3 = the main menu, main state 1 = in play). The pause menu reopens where it
# was left (possibly in a slot ring); Escape in its slot ring closes the whole menu, Left only the slot ring.
#
# Saves live in the prefix's `drive_c/users/<user>/Documents/Eidos/Tomb Raider - Underworld/` as `<AREA> - <TITLE>.TRUSave`.
# A prefix made by `winecfg` links Documents (and Desktop, Music, ...) to the Linux home directory: tru.exe would then
# read and write the user's own saves. `setup` replaces those links with empty directories and every other command
# refuses to run while any user directory of the prefix still holds such a link.
WREF_SRC=$(cd "$(dirname "$0")" && pwd); . "$WREF_SRC/env.sh"
W="$WREF_SRC/w.sh"
WUSER=$(ls "$WINEPREFIX/drive_c/users" | grep -v -x -e Public -e Default | head -1)
UDIR="$WINEPREFIX/drive_c/users/$WUSER"
SAVES="$UDIR/Documents/Eidos/Tomb Raider - Underworld"
SHOT="$OU_WREF_DIR/menu-sel.png"
k() { $W wkey key "$1" 200 > /dev/null; sleep "${2:-1.5}"; }
private() {
    [ -d "$UDIR" ] || { echo "no Wine user directory under $WINEPREFIX" >&2; exit 1; }
    links=$(find "$WINEPREFIX/drive_c/users" -maxdepth 2 -type l)
    [ -n "$links" ] && { echo "links out of the prefix (run menu.sh setup): $links" >&2; exit 1; }
    mkdir -p "$SAVES"
}
nsaves() { ls "$SAVES" 2>/dev/null | grep '\.TRUSave$' | grep -v -x Autosave.TRUSave | wc -l; }
state() { $W wmem state; }
# `ring N slot M open K`: the selected ring item, the selected slot and the item whose slot ring is open (- = none).
# In play without the pause menu (main state 1, gameTracker flag 0x10000000 clear) nothing is read from the screen.
sel() {
    st=$(state); f=$(echo "$st" | cut -d' ' -f6)
    case "$st" in "main 1 "*) [ $((0x${f:-0} & 0x10000000)) -eq 0 ] && { echo "ring - slot - open -"; return; } ;; esac
    "$WREF_SRC/shot.sh" "$SHOT.a.png" && sleep 0.3 && "$WREF_SRC/shot.sh" "$SHOT.b.png" &&
        magick "$SHOT.a.png" "$SHOT.b.png" -evaluate-sequence max -separate -evaluate-sequence min "$SHOT" ||
        { echo "ring - slot - open -"; return; }
    lit() { [ "$(magick "$SHOT" -crop "$1x$1+$(($2 - $1 / 2))+$(($3 - $1 / 2))" -format "%[fx:round(maxima*255)]" info:)" -ge 200 ]; }
    best() {
        n=0; idx=-; for xy in "$@"; do
            lit 40 "${xy%,*}" "${xy#*,}" && { [ "$idx" = - ] && idx=$n || idx=many; }
            n=$((n + 1))
        done
        [ "$idx" = many ] && idx=-; echo "$idx"
    }
    open=-
    for c in 3,480,233 4,440,315; do
        i=${c%%,*}; x=${c#*,}; y=${x#*,}; x=${x%,*}
        lit 10 $((x - 44)) "$y" && lit 10 $((x + 44)) "$y" && lit 10 "$x" $((y - 44)) && lit 10 "$x" $((y + 44)) && open=$i
    done
    echo "ring $(best 640,360 640,155 551,175 480,233 440,315 440,405 480,488 551,545) slot $(best 728,175 800,233 840,315 840,405 800,488 728,545) open $open"
}
field() { echo "$1" | cut -d' ' -f"$2"; }
# Bring the ring selection to item $1 (1..7) with the slot ring closed; 30 tries.
ringto() {
    t=0
    while [ $t -lt 30 ]; do
        s=$(sel); r=$(field "$s" 2); sl=$(field "$s" 4); op=$(field "$s" 6)
        if [ "$sl" != - ] || [ "$op" != - ]; then k 25   # leave the slot ring
        elif [ "$r" = - ]; then k 1B 3                   # in play: open the pause menu; a dialog: close it
        elif [ "$r" = "$1" ]; then return 0
        elif [ "$r" = 0 ]; then k 25
        elif [ "$r" -gt "$1" ]; then k 26
        else k 28; fi
        t=$((t + 1))
    done
    echo "ring did not reach item $1 ($s)" >&2; return 1
}
# Open the slot ring of item $1 (3 LOAD / 4 SAVE GAME) and select slot $2 (1..4); 30 tries. Return opens it with no
# slot selected (the centre drawn selected), Right enters it at slot 2.
slotto() {
    t=0
    while [ $t -lt 30 ]; do
        s=$(sel); r=$(field "$s" 2); sl=$(field "$s" 4); op=$(field "$s" 6)
        if [ "$op" = "$1" ] && [ "$sl" = "$2" ]; then return 0
        elif [ "$op" = "$1" ] && [ "$sl" = - ]; then k 27 2
        elif [ "$op" = "$1" ] && [ "$sl" -gt "$2" ]; then k 26
        elif [ "$op" = "$1" ]; then k 28
        elif [ "$op" = - ] && [ "$sl" = - ] && [ "$r" = "$1" ]; then k 0D 3
        else ringto "$1" || return 1; fi
        t=$((t + 1))
    done
    echo "slot ring did not reach slot $2 ($s)" >&2; return 1
}
# Wait (up to 10 min) for the main menu: main state 10 / front-end sub-state 3 in three samples 2 s apart; the attract
# movie (sub-states 6 / 7, after ~60 s idle) is left with Escape.
menuready() {
    i=0; n=0
    while [ $i -lt 300 ]; do
        s=$(state)
        case "$s" in
        "main 10 fe 3 "*) n=$((n + 1)); [ $n -ge 3 ] && return 0 ;;
        "main 10 fe 6 "* | "main 10 fe 7 "*) n=0; k 1B 3 ;;
        *) n=0 ;;
        esac
        sleep 2; i=$((i + 1))
    done
    echo "no main menu: $s" >&2; return 1
}
# Wait (up to 5 min) until the level runs: main state 1 with a player whose position is the same in two samples 5 s
# apart (the load is over and nothing moves her).
waitplay() {
    last=; i=0
    while [ $i -lt 60 ]; do
        s=$(state); p=${s#*pos }
        case "$s" in "main 1 "*) case "$s" in *"player 00000000"*) last= ;; *) [ "$p" = "$last" ] && break; last=$p ;; esac ;; esac
        sleep 5; i=$((i + 1))
    done
    state
    [ $i -lt 60 ] || { echo "the level did not start (menu.sh shot load)" >&2; return 1; }
}
case "$1" in
setup)
    for u in "$WINEPREFIX"/drive_c/users/*/; do
        for l in Desktop Documents Downloads Music Pictures Videos; do
            [ -L "$u$l" ] && rm "$u$l" && mkdir "$u$l" && echo "unlinked $u$l"
        done
    done
    private; echo "saves: $SAVES" ;;
install)
    private; shift
    if [ "$1" = --only ]; then shift; find "$SAVES" -maxdepth 1 -name '*.TRUSave' ! -name Autosave.TRUSave -delete; fi
    for f in "$@"; do cp "$f" "$SAVES/" && chmod u+w "$SAVES/$(basename "$f")"; done
    ls "$SAVES" ;;
boot)
    private
    had=$([ -f "$SAVES/Autosave.TRUSave" ] && echo 1)
    "$WREF_SRC/tru.sh"
    wref_display || exit 1
    # the setup dialog opens only in some prefixes (and can take minutes on a loaded machine); the game window may
    # come up directly
    i=0
    until [ $i -ge 120 ]; do
        $W wkey click "Tomb Raider: Underworld - Setup" Ok 2>/dev/null | grep -q clicked && echo "setup dialog: Ok"
        xdotool search --name '^Tomb Raider: Underworld$' > /dev/null 2>&1 && break
        sleep 5; i=$((i + 1))
    done
    [ $i -ge 120 ] && { echo "no game window after 10 min" >&2; exit 1; }
    menuready || exit 1
    if [ -z "$had" ]; then
        # "Create autosave file?" (over the main menu, NO selected): YES = Left, Return
        i=0; until [ -f "$SAVES/Autosave.TRUSave" ] || [ $i -ge 10 ]; do k 25 1; k 0D 8; i=$((i + 1)); done
        [ -f "$SAVES/Autosave.TRUSave" ] || { echo "autosave dialog not answered" >&2; exit 1; }
        menuready || exit 1
    fi
    state ;;
load)
    private; slot=${2:-1}
    menuready || exit 1
    ringto 3 && slotto 3 "$slot" || exit 1
    k 0D 4; k 25 2; k 0D 10                  # load -> "Do you wish to continue?" -> YES
    waitplay ;;
resume)
    # the centre item (RESUME GAME: the autosave, i.e. the last checkpoint) -> YES if a dialog asks; waits for play
    private; menuready || exit 1
    t=0; while [ $t -lt 10 ] && [ "$(field "$(sel)" 2)" != 0 ]; do k 27; t=$((t + 1)); done
    k 0D 6
    s=$(state); [ "$(field "$s" 2)" = 10 ] && [ "$(field "$(sel)" 2)" = - ] && { k 25 2; k 0D 6; }
    waitplay ;;
save)
    private; label=${2:?label}; slot=${3:-$(( $(nsaves) + 1 ))}
    touch "$OU_WREF_DIR/menu-save.stamp"; sleep 1
    paused() { f=$(state | cut -d' ' -f6); [ $((0x${f:-0} & 0x10000000)) -ne 0 ]; }
    paused || k 1B 3                         # the pause menu (it reopens where it was left)
    ringto 4 && slotto 4 "$slot" || exit 1
    # Return on an empty slot opens "Enter savegame name" ("Savegame <n>", OK selected; a used slot asks to overwrite,
    # NO selected): Return writes `Savegame <n>.TRUSave`, then "Save Successful" (OK)
    k 0D 4; new=; i=0
    while [ $i -lt 12 ]; do
        new=$(ls -t "$SAVES" | grep '\.TRUSave$' | grep -v -x Autosave.TRUSave | head -1)
        [ -n "$new" ] && [ "$SAVES/$new" -nt "$OU_WREF_DIR/menu-save.stamp" ] && break
        new=; k 0D 4; i=$((i + 1))
    done
    mkdir -p "$OU_WREF_DIR/saves"
    if [ -n "$new" ]; then
        cp "$SAVES/$new" "$OU_WREF_DIR/saves/$label.TRUSave" && echo "saved $new -> saves/$label.TRUSave"
        k 0D 3                               # Save Successful: OK
    else
        echo "no new save (menu.sh shot save)" >&2
    fi
    # close the pause menu (Escape in its slot ring closes all of it)
    t=0; while [ $t -lt 5 ] && paused; do k 1B 3; t=$((t + 1)); done ;;
sel)
    sel ;;
shot)
    "$WREF_SRC/shot.sh" "$OU_WREF_DIR/${2:?name}.png" ;;
*)
    sed -n '2,14p' "$0"; exit 2 ;;
esac
