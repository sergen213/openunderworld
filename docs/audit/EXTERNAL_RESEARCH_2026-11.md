# External research audit (2026-11-16)

Scope: external references that are **not already in** [RESEARCH.md](../RESEARCH.md) §4 ("Sources"), plus a check on
what RESEARCH.md says about editions. RESEARCH.md already covers and verifies TRLAU-menu-hook, ModLoader,
BigfileLogger, Yura, Templates, cdcEngineTools, cdcResearch, cdcEngine, cdcEngineDXHR, TR7AE-level-viewer,
Gibbed.CrystalDynamics, cdcengine.re, vgmstream `mul.c`, PCGamingWiki, GOGDB, WSGF, ModDB, tombraidermodding.com,
Wikipedia and the GOG many-core thread. Those rows are not repeated here.

Tags: **[O]** observed (page opened 2026-11-16, or a repo file/command cited), **[I]** inference.
Every URL in the table was opened in this pass. Licence "none" = no licence file, reference only.

## 1. New references

| # | Reference | URL | Scope | OU subsystem | Licence | In RESEARCH.md? | Verification |
|---|---|---|---|---|---|---|---|
| 1 | cdcEditor (Raq1), Blender add-on | https://github.com/Raq1/cdcEditor | Python import/export of TR7/TRAE/TR8 models, levels, animation, cutscenes (`core/cine_format.py`), cloth (`core/cloth_simulation.py`), drawgroup visibility, markup, `.mul` subtitles, FSFX render effects; per-platform bigfile file lists incl. `Tomb Raider Underworld (PC).txt` and six console TR8 lists | formats (Model, Animation, Cinematic, Markup), render (drawgroups, FSFX), tools | MIT | No (RESEARCH cites only Raq's older Noesis plugin via tombraidermodding) | [O] README: TR8 PC support is "Model ✓~, Animation ✓✓, Cutscene ✓", **no TR8 level or audio**. Not yet cross-checked against our parsers. Highest-value new source: MIT, so code can be studied/ported with attribution. |
| 2 | TRAE-menu-hook (fork aman-11111; original TheIndra55 name before the TRLAU rename) | https://github.com/aman-11111/TRAE-menu-hook | Older snapshot of the menu hook with plain-text address notes `docs/tr8.txt`, `docs/notes.txt`, `game/d3d/d3dterrain.*`, `game/reloc.*`, `game/script/script.*` | RE (address names), formats (relocs), script | GPL-3.0 | Indirectly (successor TRLAU is used) | [O] file list/README only. `docs/tr8.txt` not yet diffed against `tools/ghidra/names/tru_gog.csv`. README: "Game no longer crashes while DEP is enabled" — same fix GOG later shipped as `DEPHook.asi` (RESEARCH §1). |
| 3 | CDCE.TIGER.Tool (Ekey) | https://github.com/Ekey/CDCE.TIGER.Tool | TIGER archives + DRM v22–v24 dumper for TR2013..Avengers (Foundation era) | formats (lineage only) | none in repo | No | [O] Supports only post-2013 games; DRM v22+ ≠ TR8 (cdcEngineTools expects version 19 for TR8, see #4). Low relevance; useful only to show the format's later evolution. |
| 4 | cdcEngineTools issue #10 "Tomb Raider Underworld .drm" | https://github.com/Gh0stBlade/cdcEngineTools/issues/10 | User reports for `DRM.exe` "DRM v1.2 for Tomb Raider: Underworld, PC (PC-W)" | formats (DRM/CDRM) | n/a | Tool cited, issue not | [O] Tool prints "expected: 19 got: 1297237059". 1297237059 = 0x4D524443 = ASCII `CDRM` little-endian [O arithmetic]: the tool rejects compressed CDRM containers and needs pre-decompressed DRMs. Agrees with our split CDRM → DRM path (docs/formats). DRM version 19 for TR8 is a checkable constant [I: confirm against our DRM parser header check]. |
| 5 | GDC 2013, Jason Lacroix (Crystal Dynamics), "Casting a New Light on a Familiar Face: Light Based Rendering in Tomb Raider" | https://gdcvault.com/play/1018146/Casting-a-New-Light-on | Official talk: "evolution from the lighting system used in Tomb Raider: Underworld to the system used in Tomb Raider (2013)"; free slides (pptx on media.gdcvault.com) | render (lighting model, light pre-pass vs forward, effects) | proprietary talk; facts only | No | [O] abstract only (slides not downloaded in this pass). Only first-party source on TRU's lighting found. Our docs describe tru.exe's lighting from shaders/decomp (RENDERING.md, SHADERS.md); compare the slides' TRU section against that. |
| 6 | Speed Demos Archive, TRU run comments (Jarek Hanzelka, 2009-02-20, PC) | https://speeddemosarchive.com/TombRaiderUnderworld.html | Player-observed mechanics and glitches on 1.0/1.1 era PC | gameplay (falls, swan dive, wall jump, triggers, AI, reload), save/load | n/a | No | [O] Claims, unverified against tru.exe: (a) stumble on survivable landings cancelled by holding sprint+forward during the fall; (b) swan dive from survivable height does less damage than a jump; (c) "crouch drop bug" survives any height — **PC only**; (d) mid-air swan dive input window gets harder above ~25–30 fps (frame-rate-dependent input); (e) weapons reload on holster/redraw; (f) full health on reload; (g) wall jumps extend by swan diving between walls; (h) sprinting into some cutscene triggers forces a slow run; (i) Jan Mayen: Valhalla starts when the *last* of four wheels turns, any order; (j) enemies respawn at original positions on checkpoint reload. See leads. |
| 7 | Speedrun.com TRU guides index | https://www.speedrun.com/tru/guides | Guides: common-issue fixes, autosplitter/load remover, IL retiming, Jan Mayen gate skip; Discord link | gameplay (route/skip reproduction), timing | n/a | No | [O] index only; the autosplitter guide implies community memory addresses for load/level state [I] — a possible cross-check for our Ghidra names. |
| 8 | Tomb Raider Chronicles, TRU bugs list (compiled by Cog, TRF) | https://www.tombraiderchronicles.com/underworld/walkthrough/bugs.html | ~40 general + location-specific bugs, marks which were fixed by PC 1.1 | gameplay fidelity, scripting, save/load, physics | n/a (all rights reserved) | No | [O] Relevant for "do not fix original bugs silently" decisions: e.g. G19 real-time cutscene skip oddities; S12 sprinting past the Croft Manor section-load trigger leaves no level; yacht: "Lara floats when yacht comes down and bends her legs when it goes up" (matches our dry-deck/buoyancy work, GAMEPLAY.md water rows); 5:4 resolution stretches; AF ineffective on PC. Xibalba ramp (S2) and Kraken lever (S8) confirmed fixed in 1.1, consistent with RESEARCH §3 patch notes and `patch.000` replacing `l2_puzzleroom.drm` [O RESEARCH.md:125]. |

Opened but not usable (blocked by Cloudflare, HTTP 403): TRF "GOG Update - Tomb Raider Legend"
(`https://www.tombraiderforums.com/showthread.php?t=230453`, reportedly a WinMerge diff of all three LAU GOG updates)
and SteamDB depot 8141 (`https://steamdb.info/depot/8141`, Steam TRU content manifest). Neither is in the table.

Searched without a usable hit: noclip.website support for any LAU game (no result naming it); Xentax TRU threads
(Xentax is offline; ResHax only has reboot-era threads).

**Previous native ports of TRU (brief Phase 0.1).** First searched 2026-11-16 (queries not recorded then; result
"none found"), repeated 2026-11-20 with web search, queries `"Tomb Raider Underworld" native Linux port OR
reimplementation OR decompilation github` and `"Tomb Raider: Underworld" source port open source engine recreation
tru.exe` (20 hits; the result list read, and the trudlcport fork's docs/RELATED_WORK.md opened). Result: **no native
port, engine reimplementation or decompilation of TRU other than OpenUnderworld itself was found** [O]. What the
searches return instead:

| Project | What it is | Why it is not a TRU port |
|---|---|---|
| TheIndra55/cdcEngine (https://github.com/TheIndra55/cdcEngine) | decompilation of Tomb Raider: Legend (GPL-3.0) | Legend only (already cited in RESEARCH.md) |
| rrika/cdcEngineDXHR (https://github.com/rrika/cdcEngineDXHR) | non-matching decompilation of Deus Ex: Human Revolution, Linux via dxvk-native | a later cdc game, not TRU |
| andrearicci93/trudlcport and its fork LukasKurthRocks/tomb-raider-underworld-dlc-pc (https://github.com/LukasKurthRocks/tomb-raider-underworld-dlc-pc) | Xbox 360 DLC content brought to the PC game as ASI plugins and data conversion | keeps and patches tru.exe (a content port); its docs/RELATED_WORK.md (2026-10-29) lists OpenUnderworld as the only TRU engine reimplementation |
| opentomb/OpenTomb, LostArtefacts/TRX, xythobuz/OpenRaider | open engines for the Core Design games (TR1-5) | different engine family |
| caniplayonlinux.com, CodeWeavers, PCGamingWiki TRU pages | run tru.exe through Proton / Wine / CrossOver | compatibility layers, not ports |

## 2. Executable versions and detection (ROADMAP milestone 8)

Known PC builds (RESEARCH.md §2–§3, re-checked; no new published hashes were found online):

| Build | Detect by | Status |
|---|---|---|
| Demo 2008-10-31 (Steam demo AppID 8150) | — | no hash known |
| Retail 1.0 (SecuROM 7.38.0015; PL/CZ/HU 7.38.0014; GfW Marketplace SecuROM PA 7.42) | SecuROM sections, FileVersion 1.0 [I] | no hash known |
| Retail 1.1 disc patch 2008-12-16 | FileVersion 1.1, still SecuROM-wrapped [I] | no hash known |
| Steam AppID 8140, depot 8141 ("v1.1", steamless-strippable) | Steam stub; code layout after unwrapping probably = GOG (TRLAU uses one TR8 address set for Steam and GOG) [I] | depot manifest blocked (403) |
| GOG "1.1" 2021-05-06, GOG "1.1 GOG v3" 2024-11-07 | **ours**: `tru.exe` 6,422,528 bytes, SHA-256 `1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011` [O `sha256sum`, `ls -l`; docs/re/BINARIES.md §1], PE TimeDateStamp 2008-12-16 17:31:02 UTC, VERSIONINFO 1.1, no DRM [O docs/re/EXECUTABLE.md:12-31] | verified |

Detection recipe for other editions (proposal, [I]): (1) SHA-256 allow-list (only GOG known); (2) PE TimeDateStamp +
FileVersion; (3) presence of SecuROM / Steam stub sections → refuse or ask for a stripped exe; (4) the byte-pattern
anchors of RESEARCH.md §2 table (TRLAU/ModLoader/BigfileLogger patterns match once each in ours) as a layout check
independent of the wrapper. Note that OpenUnderworld needs `tru.exe` only for tooling/RE, not for running data, so
milestone 8 is mostly about **data** differences (bigfile/patch.000 lists, regional `pc-pch`/`pc-russ` lists in
Gibbed, already cited); cdcEditor (#1) ships a TR8 PC bigfile file list usable as a second name list for edition diffs.

## 3. Leads (to verify against tru.exe / our code; none confirmed)

1. **Stumble cancel by sprint+forward** (#6a): GAMEPLAY.md:262 documents fall damage thresholds but not whether held
   input skips the hard-landing recovery. Check the landing state's exit conditions in tru.exe (FallState 0x75C550 →
   landing 0x75D080).
2. **Swan dive landing damage lower than jump** (#6b): our fall damage is height-only (GAMEPLAY.md:262, "never scaled");
   check whether swan-dive landing uses a different height origin or curve.
3. **PC-only "crouch drop" no-damage bug** (#6c): suggests the fall height origin (+0x6DC set on FallState entry) is
   reset by a crouch/ledge-drop path on PC. If real, faithful behaviour would reproduce it.
4. **Frame-rate-dependent input windows** (#6d) and RESEARCH's high-FPS physics notes: our sim is fixed-rate; record
   which windows tru.exe measures in frames vs seconds.
5. **Holster = reload, full health on reload** (#6e/f): check against Combat/save code.
6. **Jan Mayen wheel order** (#6i): Valhalla trigger on 4th wheel in any order — check the level script (SCMD VM).
7. **Section-load trigger skippable by sprint** (#8 S12) and slow-run inside cutscene triggers (#6h): trigger volumes
   tested per frame against a moving point [I]; a native port with fixed tick may differ.
8. **Yacht leg bend/float** (#8): the original shows feet floating/bending on the moving deck — the reference behaviour
   is imperfect, relevant when judging our dry-deck rule.
9. **GDC 2013 slides** (#5): download and compare TRU lighting description with RENDERING.md / SHADERS.md.
10. **cdcEditor** (#1): diff its TR8 animation, cine, markup and cloth readers against `src/formats`; MIT allows reuse.
11. **TRAE-menu-hook `docs/tr8.txt`** (#2): diff names into `tools/ghidra/names/tru_gog.csv` candidates.
12. **DRM version 19** (#4): assert in our DRM parser if not already.
