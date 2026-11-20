#!/bin/sh
# l4_ballcourt_combat.sh [part]: chapter 4 from a tru.exe save, l4_ballcourt's "combat 4" (panther 6532, poachers
# 6537..6539) born by Lara walking across trigger plane 6543, and the combat traces of docs/REFERENCE_COMPARISON.md
# (M31 / M40..M46, 2026-11-20).
#
#   SAVE=<chapter-4 .TRUSave> sessions/l4_ballcourt_combat.sh start   compositor, private Documents, boot, LOAD GAME
#                                                                     slot 1, goto l4_ballcourt (the checkpoint there
#                                                                     becomes the autosave), then `cross`
#   sessions/l4_ballcourt_combat.sh again     restart tru.exe, RESUME GAME (that checkpoint: nothing born), `cross`
#   sessions/l4_ballcourt_combat.sh cross     walk over 6543, then `idle`
#   sessions/l4_ballcourt_combat.sh idle      70 s standing just past the plane: births, the panther, the poachers
#   sessions/l4_ballcourt_combat.sh pistols   on the lower court near the panther: G (target lock) + H (shoot) 8 s
#   sessions/l4_ballcourt_combat.sh save      a tru.exe save of the running state -> $OU_WREF_DIR/saves/ (menu.sh save)
#
# Why a save: a `wmem goto` straight after a new game runs with the prologue's chapter state; a chapter-4 save gives
# tru.exe the chapter's script globals and gear. l4_ballcourt's enemies are dormant intros (+0x72 & 0x80, tru.exe
# 0x5C2360) that trigger planes birth through dispatchers (instanceref Birth 0x4AC0D0): 6546 (the way in from
# l4_trailtube) the dead poachers 6540..6542, 6543 ("combat 4") panther 6532 + poachers 6537..6539, 6544 panther 6534.
# 6543 is one-sided: only a move towards its local +Y (world (0.891, -0.454), i.e. east, coming from l4_templepath)
# fires it, and a teleport (`ref tpv`) over it does not. Teleport with `ref tpv` (velocity zeroed): a `pole lara` jump
# of tens of thousands of units keeps her velocity, flings her and can kill her; a death reloads the checkpoint.
# Frame: tru.exe = ours + (5881.3, -10787.3, -15435.9) after the goto and after loads of saves made in it (planes 6543 /
# 6546, dispatcher 6535; headings: tru yaw = ours + 90 degrees).
WR=$(cd "$(dirname "$0")/.." && pwd); . "$WR/env.sh"
W="$WR/w.sh"; M="$WR/menu.sh"; O=$OU_WREF_DIR
DX=5881.3 DY=-10787.3 DZ=-15435.9
tru() { awk -v x="$1" -v y="$2" -v z="$3" -v dx=$DX -v dy=$DY -v dz=$DZ 'BEGIN { printf "%.1f %.1f %.1f", x + dx, y + dy, z + dz }'; }
enemies() { $W enemy list | grep -E 'cat_|poacher'; }
restart() { systemctl --user stop "$WREF_TRU_UNIT" 2>/dev/null; wineserver -k 2>/dev/null; sleep 3; $M boot > /dev/null || exit 1; }
case "${1:-start}" in
start)
    "$WR/start-kwin.sh" || exit 1
    $M setup
    [ -n "$SAVE" ] && $M install --only "$SAVE"
    $M boot > /dev/null && $M load 1 || exit 1
    $W wmem goto l4_ballcourt > /dev/null; sleep 90
    $W wmem state
    $W pole dump 6535 6543 6546 | tee "$O/bc-frame.txt"
    "$0" cross ;;
again)
    restart; $M resume || exit 1
    $W pole dump 6543 | tee "$O/bc-frame.txt"
    "$0" cross ;;
cross)
    # 600 units on the local -Y side of 6543 at the floor (ours (-32783, 3585), floor 3076); the same walk as ours
    # (`YAW=-0.471`, forward 3.3 s)
    enemies > "$O/bc-enemies-before.txt"; echo "enemies before: $(wc -l < "$O/bc-enemies-before.txt")"
    $W ref tpv $(tru -32783 3585 3136) 1500 > /dev/null; sleep 1
    (HEAL=300 timeout 80 $W enemy fight 70000 5 6532 6537 6538 6539 > "$O/bc-idle.txt" &)
    sleep 0.3
    CAMFOLLOW=0.891,-0.454,600,250 CAMUNTIL=3300 $W ref trace 3500 26:0:3300 > "$O/bc-cross.txt"
    $W wmem state; enemies | tee "$O/bc-enemies-after.txt"
    sleep 75; grep -E 'hurt|hit |heal' "$O/bc-idle.txt" ;;
pistols)
    # Lara on the lower court (ours (-20981, 9412), she drops ~900 to its floor), the panther roaming there: lock on and
    # shoot; hits from the enemy health words, shots alternate hands
    $W ref tpv $(tru -20981 9412 3575) 1500 > /dev/null; sleep 3
    (HEAL=300 timeout 30 $W enemy fight 20000 5 6532 6537 > "$O/bc-pistols.txt" &)
    sleep 3; $W ref trace 9000 47:0:8500 48:800:8000 > "$O/bc-pistols-keys.txt"
    sleep 9; grep -E 'hurt|hit |heal' "$O/bc-pistols.txt" ;;
save)
    $M save "l4_ballcourt-${2:-combat4}" ;;
*)
    echo "unknown part $1" >&2; exit 2 ;;
esac
