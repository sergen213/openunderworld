#!/bin/sh
# Mover evaluation runs (docs/formats/PHYSICS.md "Character controller"): per mover (OU_MOVER=sweep|jolt|jolt-capsule)
# the traces analyse.py reads (stairs and hearth from GameReal.MoverStairsTrace, the two replays and the tick costs from
# ou-game), the riser survey and the five self-tests. Needs a
# build with the evaluation switch (the commit PHYSICS.md names), OU_GAME_DIR and a display: run it inside a private
# compositor, e.g.
#   dbus-run-session -- env -u DISPLAY kwin_wayland --virtual --socket ou-mover --exit-with-session tools/mover_eval/eval.sh
# Output: $OU_EVAL_DIR (default build/mover_eval); then tools/mover_eval/analyse.py [$OU_EVAL_DIR].
# The Scene-level tools (cv_steps / cv_slope / cv_cost / find_ramps .cpp) build against the static libraries:
#   g++ -std=c++23 -O2 -Isrc -I<glm> tools/mover_eval/cv_steps.cpp -o cv_steps libou_physics.a libou_collision.a libou_formats.a libou_cdc.a libJolt.a -lz
WT=$(cd "$(dirname "$0")/../.." && pwd)
W=${OU_EVAL_DIR:-$WT/build/mover_eval}
E=$WT/tools/mover_eval
G="$WT/build/ou-game --game-dir \"$OU_GAME_DIR\" --renderer opengl --no-audio --no-movies --no-cinematics --no-overlay --draw-last --sync-loads --width 160 --height 120"
run() { # name mover args...
  name=$1; m=$2; shift 2
  OU_MOVER=$m OU_MOVER_LOG=$W/$name.$m.trace XDG_DATA_HOME=$W/xdg.$m sh -c "$G $*" > $W/$name.$m.log 2>&1
  echo "$name $m exit $?"
}
mkdir -p $W
for m in sweep jolt jolt-capsule; do
  OU_MOVER=$m OU_MOVER_LOG=$W/gtest.$m $WT/build/ou_game_tests --gtest_filter='GameReal.MoverStairsTrace:GameReal.MoverRiserSurvey' > $W/gtest.$m.log 2>&1
  run pool_to_ragnarok $m "--level 'pc-w\\l4_pool.drm' --input-script $WT/tests/replays/pool_to_ragnarok.txt --frames 180"
  run save_move_load $m "--level 'pc-w\\l4_pool.drm' --input-script $WT/tests/replays/save_move_load.txt --frames 240"
  run cost_pool $m "--level 'pc-w\\l4_pool.drm' --input-script $WT/tests/replays/pool_to_ragnarok.txt --frames 600"
  run cost_junglehub $m "--level 'pc-w\\l2_junglehub.drm' --input-script $E/fwd.txt --frames 600"
  (cd $WT/build && OU_MOVER=$m ctest -j2 -R 'game_selftest_(locomotion|ledge|traversal_chain|fall_damage|sprint)$' > $W/selftest.$m.log 2>&1)
  echo "== self-tests $m"; grep -E "Test +#|tests passed" $W/selftest.$m.log
done
