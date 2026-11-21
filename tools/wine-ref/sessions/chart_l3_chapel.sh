#!/bin/sh
# Chart reference corpus, l3_chapel balance beam 1031 (63 wide, axis x 4708.6 ours, Lara's root on it at z -6163) with
# ledge loop 1009 round its top (x 4740.6 / 4677.4 ours) (GameReal.BeamFallsCatchScaffoldLedge,
# ChapelHangClimbsOntoBalanceBeam). The offset is measured after the goto from the instances present in both frames
# (cmeasure; the 2026-11-21 first run used "tru start - our intro", which put her elsewhere). Spawns are given in our
# frame (tru()).
#   beam: from the hang on 1009's +x side (pinned 600 over the hang root, 6 out: the fall's air grab), Jump climbs
#   onto the beam (S439 T698 -> S882 -> S885 by the chart); the beam cases chain their keys after that (tick 200+),
#   facing -y along the beam (camera (0,-1) is set after the climb by turning: the beam faces -y, Lara faces it).
#   drop: pinned 23 over the beam's axis facing -y: lands on it (or grabs the loop).
# Usage (session dir exported, see chart_lib.sh): sessions/chart_l3_chapel.sh [noload]
WREF_SRC=$(cd "$(dirname "$0")/.." && pwd); . "$WREF_SRC/env.sh"; wref_private_prefix
. "$WREF_SRC/sessions/chart_lib.sh"
UNIT=l3_chapel
[ "$1" = noload ] || cunit $UNIT || exit 1
cmeasure $UNIT || exit 1
HOLD=${HOLD:-60}
F=beam
H=$(tru 4907.6 4594.8 -6063 180); CH=-1,0          # 600 over 1009's +x hang root (161 out of x 4740.6), facing -x
ON="20:120:124"                                    # Jump from the hang: onto the beam
ccase $F ch_loop_hang $H 200 $CH "pinned 600 over loop 1009's +x hang root: falls, air grab"
ccase $F ch_beam_climb_on $H 300 $CH "hang on 1009, Jump 120..124: bbeam_ledgeu_climbup onto beam 1031, beam idle" $ON
ccase $F ch_beam_walk $H 520 $CH "on the beam (Jump 120), then Left held 220..480: walks along the beam (camera -x: Left = -y)" $ON 25:220:480
ccase $F ch_beam_walk_back $H 520 $CH "on the beam, then Right held 220..480 (+y)" $ON 27:220:480
ccase $F ch_beam_across_fwd $H 700 $CH "on the beam, then Up held 220..660 (across the beam, -x): off balance, about to fall, fall / catch" $ON 26:220:660
ccase $F ch_beam_across_back $H 700 $CH "on the beam, then Down held 220..660 (across, +x)" $ON 28:220:660
ccase $F ch_beam_walk_across $H 700 $CH "on the beam, Left held 220..660 + Up held 260..660: walking with the stick across: about to fall" $ON 25:220:660 26:260:660
ccase $F ch_beam_recover $H 700 $CH "on the beam, Left 220..600 + Up 260..360 then Down 360..460: off balance and recover" $ON 25:220:600 26:260:360 28:360:460
ccase $F ch_beam_sprint $H 560 $CH "on the beam, Left + Shift held 220..500: beam sprint" $ON 25:220:500 a0:220:500
ccase $F ch_beam_jump $H 420 $CH "on the beam, Jump 240..244: jump on / off the beam" $ON 20:240:244
ccase $F ch_beam_jump_along $H 460 $CH "on the beam, Left 220..300 + Jump 250..254: jump along the beam" $ON 25:220:300 20:250:254
ccase $F ch_beam_crouch $H 420 $CH "on the beam, Crouch 240..244: beam drop (bbeam_drop_* to the hang)" $ON 11:240:244
ccase $F ch_beam_crouch_walk $H 460 $CH "on the beam, Left 220..400 + Crouch 260..264: drop while walking" $ON 25:220:400 11:260:264
B=$(tru 4708.6 4594.8 -6140 -90); CB=0,-1
ccase $F ch_beam_drop_on $B 260 $CB "pinned 23 over the beam's axis facing -y, released: lands on the beam or grabs the loop"
ccase $F ch_beam_drop_on_walk $B 400 $CB "dropped onto the beam, then Up held 120..360 (along -y)" 26:120:360
