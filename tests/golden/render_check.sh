#!/bin/sh
# Render regression checks on real game data (ctest render_*, docs/RENDERING.md "Testing").
#   render_check.sh GAME GOLDEN_TOOL OUTDIR GOLDENDIR MODE ...
#     ao RENDERER            AO off vs CORTAO on l3_main_bedroom: CORTAO must darken the frame within bounds
#     ao_rt                  AO off vs ray-traced AO (Vulkan, RT shadows); 77 without ray queries
#     ao_parity              OpenGL vs Vulkan frames with CORTAO on (OUTDIR images of the two `ao` runs)
#     golden NAME LEVEL ARGS stored golden tests/golden/NAME.png vs a fresh frame; OU_GOLDEN_BLESS=1 rewrites it
# Every frame is deterministic: --render-hz 60 (fixed virtual frame time), --sync-loads, 120 frames from the level's own
# start checkpoint, an empty config (XDG dirs in OUTDIR), 640x360. Exit 77 = skipped (no OU_GAME_DIR, display or Vulkan).
game=$1 tool=$2 out=$3 goldens=$4 mode=$5
shift 5
[ -n "$OU_GAME_DIR" ] && [ -n "$DISPLAY$WAYLAND_DISPLAY" ] || { echo "no OU_GAME_DIR or display"; exit 77; }
mkdir -p "$out/cfg" "$out/data" "$out/state" || exit 2
export XDG_CONFIG_HOME="$out/cfg" XDG_DATA_HOME="$out/data" XDG_STATE_HOME="$out/state"

# capture NAME LEVEL ARGS: out/NAME.ppm + out/NAME.log; 77 when a Vulkan run finds no device
capture() {
    name=$1 level=$2
    shift 2
    rm -f "$out/$name.ppm"
    "$game" --game-dir "$OU_GAME_DIR" --level "pc-w\\$level.drm" --render-hz 60 --sync-loads --frames 120 \
        --no-audio --no-movies --no-cinematics --no-overlay --width 640 --height 360 --screenshot "$out/$name.ppm" "$@" \
        > "$out/$name.log" 2>&1
    code=$?
    grep -E '^ou-game: (AO |final tick)' "$out/$name.log"
    if [ $code -ne 0 ] || [ ! -f "$out/$name.ppm" ]; then
        case " $* " in *" vulkan "*) grep -q '^ou-game: Vulkan ' "$out/$name.log" || { echo "$name: no Vulkan device"; exit 77; } ;; esac
        tail -n 20 "$out/$name.log"
        echo "$name: ou-game exit $code"
        exit 1
    fi
}

case $mode in
ao) # Measured on RX 7900 XTX / Mesa 26.2.6 (2026-11-16): luminance drop GL 0.289, Vulkan 0.286 (/255, whole frame,
    # bit-identical between reruns); AO only takes ambient light, so (almost) nothing may brighten. 0.20..0.45 fails AO
    # off, AO at half strength (measured 0.14) and a doubled strength.
    r=$1
    capture "ao_${r}_off" l3_main_bedroom --renderer "$r" --preset enhanced --rt off --ao off
    capture "ao_${r}_on" l3_main_bedroom --renderer "$r" --preset enhanced --rt off --ao cortao
    grep -q '^ou-game: AO CORTAO' "$out/ao_${r}_on.log" || { echo "CORTAO not active"; exit 1; }
    exec "$tool" darker "$out/ao_${r}_off.ppm" "$out/ao_${r}_on.ppm" 0.20 0.45 ;;
ao_rt) # Measured drop 0.541 (RT AO, RT shadows). Without ray queries `--ao rt` draws CORTAO, so that case skips.
    capture ao_rt_off l3_main_bedroom --renderer vulkan --preset enhanced --rt shadows --ao off
    grep -q 'ray queries yes' "$out/ao_rt_off.log" || { echo "no ray-query device"; exit 77; }
    capture ao_rt_on l3_main_bedroom --renderer vulkan --preset enhanced --rt shadows --ao rt
    grep -q '^ou-game: AO ray traced' "$out/ao_rt_on.log" || { echo "RT AO not active"; exit 1; }
    exec "$tool" darker "$out/ao_rt_off.ppm" "$out/ao_rt_on.ppm" 0.38 0.85 ;;
ao_parity) # Measured GL vs Vulkan with CORTAO 0.072 /255, 53.6 dB (AO off 0.045); one backend without AO: >= 0.31.
    exec "$tool" compare "$out/ao_opengl_on.ppm" "$out/ao_vulkan_on.ppm" 0.15 48 ;;
golden) # Same-machine reruns are bit-identical; the limits leave room for driver drift but not for AO going missing
    # (0.285 /255) or the water mode switching (29 /255 at the cliffclimb yacht).
    g=$1 level=$2
    shift 2
    capture "golden_$g" "$level" "$@"
    if [ "$OU_GOLDEN_BLESS" = 1 ]; then exec "$tool" bless "$out/golden_$g.ppm" "$goldens/$g.png"; fi
    [ -f "$goldens/$g.png" ] || { echo "no golden $goldens/$g.png (bless: OU_GOLDEN_BLESS=1)"; exit 1; }
    exec "$tool" compare "$goldens/$g.png" "$out/golden_$g.ppm" 0.12 48 ;;
*)
    echo "render_check.sh: unknown mode $mode"
    exit 2 ;;
esac
