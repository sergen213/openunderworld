# WaterEntry agent notes (branch agent/WaterEntry; delete before merge)

## Bugs (user)
1. Jump/fall into water: too deep, then back up. Cause: SwimSplash kept e^-3t of the plunge (762 fall ~930 deep), then Surfacing at swim speed 1348/s.
2. Swan dive "slammed onto the surface": SwimSplash(swim_swan_land) zeroed velocity and used only clip root (13 u/frame) after a 3000+ u/s plunge.

## Wine measurements (2026-11-15, l2_cliffclimb, tru = ours + (12446.2, 59357.3, -19003.8) from water volume 2563)
Tools/traces: /mnt/1st4TB/Projects/openunderworld-work/waterentry-wine
- weref.c/.exe `drop x y z pinms tracems vx vy vz [vk:down:up..]` (env YAW, CAMFOLLOW, LIFT=ms:dz): per-frame pos, vel +0x2A0,
  chart state request/parent (state struct +4/+6 at *(player+0xF10)+0x510), water top pd+0x8E0, wave surface (sin, see below).
- start.sh (kwin + new game + goto l2_cliffclimb), R.sh wrapper. ALWAYS `flock ../wine.lock`, stop tru.exe after.
- Traces: d2.txt (fall 762 over top), h300, h3000, h6000 (falls), rj1 (running jump off bow), sw2/sw4 (swan dives off bow; sw4 lifted +1000),
  sw3 (swan hit a rock -> surface group), d3 (13 s treading: wave law fit).
- Scripts: resurf.py, validate.py (decoded law vs traces: max diff 1.0/2.4/0.4 for d2/h3000/h6000), swanfit.py, flat2.py (still-water refs),
  fitfree.py (waves are SIN).
Facts:
- Entry: frame after root < volume top (+0x58, no waves; 0x5D77E0/0x5D3860). First state frame: pos += v*tm only.
- S104 (swim_jump_land, req 895, parent S102 props 1307=30): v *= 0.875 per 60Hz frame, all axes, trapezoid.
  Actions table 0x8B1550: 1301=0x76D710 (to wave surface-44.1: up <= 1307*tm=15, down <= data float +0xEC(-6.5)*3.1605/tm=41.09),
  1302=0x75DC20 (down to wave surface <= 15 while above). Actions before velocity move. S104 lasts 1.0995 s (clip from frame 0).
  Then surface group S111 (rq 893), still rising 15/frame if under.
- Depths (root under wave surface): fall 762 -> 119, 3000 -> 367, 6000 -> 783; running jump off bow -> 66.
  Still water (model): 177 / 496 / 819.
- S105 swan (req 897, props 1307=-100, only action 1302 w/o target): -50/frame for 12..13 frames (stop rule not decoded; ours 0.2 s),
  + clip root from frame 0, same velocity drag; 1.033 s then S154 submerged. Off bow: 1170 under at 1 s.
  Hitting rock/floor -> T895 CheckProxySupport -> S115 surface group (rises).
- Waves: tru 0x69DA80 = sum h (sin(phi)-1) (ours cos). Reported to Main; YachtBounce owns that fix. Not changed here.

## Code (done)
- Game.hpp Tuning: waterDrag, splashRise, splashSinkCap, swanSink, swanSinkSeconds; swimSlide clamp param; waterEntry().
- Player.cpp: enterWater airborne trigger at top, clipTime=0; waterEntry(); Surfacing rises 900 u/s; swim exit uses top during SwimSplash.
- SelfTest.cpp: probe water.depth. docs/SELFTEST.md, docs/GAMEPLAY.md ("Water entry").
- Tests: GameReal.WaterEntryDepthAndRiseMatchTru (new), SwanDiveIntoWaterAndOntoLand, CliffSwanDiveIntoTheSea (updated).
- Self-tests: tests/selftests/water_entry.json (new), swan_dive.json (tightened).

## Verify
- `OU_GAME_DIR=... build-WaterEntry/ou_game_tests --gtest_filter='*Swim*:*Swan*:*Water*:*Splash*:*Dive*:*Pool*:*Sea*'` (52 passed).
- Self-tests via ../waterentry-shots/st.sh in a private kwin (see that script).
