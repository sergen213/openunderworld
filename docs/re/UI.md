# UI, menus, HUD and PDA (tru.exe)

Static reconstruction of tru.exe's UI layer: the front-end state machine, the script-driven screens, the cdc
widget classes and their draw passes, the HUD and PDA hosts. Fonts and text drawing are in
[ENGINE_ARCHITECTURE.md §21](ENGINE_ARCHITECTURE.md) and docs/formats/TEXT.md; menu layouts as measured from
captures are in docs/GAMEPLAY.md "Menus" / "HUD" / "PDA".

Evidence tags as in [CAMERA.md](CAMERA.md): **V** instruction-level (objdump), **D** decompiler only, **[I]** inference.

## 1. Architecture in one paragraph

Every menu and the HUD is a **cdc script object**. A native "std screen" (`cdc::ScriptStdScreen`, vftable 0x847C58,
8 slots) knows which script type to instantiate from a DTP id in the global data block (`*0xE7EE50`), creates it
through the script manager and keeps the instance at +4 (0x4C7BC0, D). The script type derives from `NsUIScreen`
(vftable 0x847724, 37 slots) and is itself a widget tree (`cdc::ScriptUIScreen` = `cdc::UISuperWidget` →
`cdc::UIWidgetBase`); the screens' behaviour (menu items, transitions, timelines) is script bytecode plus widget data,
not C++. The C++ side provides the widget classes, their properties / timelines, and ~60 `NtUI*` native script types
(rtti.tsv) the scripts call (dialogs, menus, save/load, display settings, game data).

## 2. Function table

| Address | Name (tru_gog.csv) | Role | Conf. | Check |
|---|---|---|---|---|
| 0x5DAE20 | MAIN_FrontEnd | main state 10: front-end sub-states 0..8 (§3) at byte 0xFAE8DA; the main menu loops the Bink movie `medsea_loop` behind the menu (0x4A12B0) | high | D |
| 0x77CB20 | FrontEnd_StateName | debug names of the sub-states (§3) | high | D |
| 0x5E0090 | MAIN_ProcessMainState (existing) | case 8 sets `mainTracker+0xC = 1` (quit) | high | V (5E0125) |
| 0x77C190 | UIGameData_QuitGame | `mainTracker = 8` after two UI resets and 0x4A0F10; script native `uigamedata` fn 45 (thunk 0x7A0F30) | high | V (77C19F), script_natives.tsv |
| 0x4C7BC0 | ScriptStdScreen_Create | vf 1: resolve the screen's script type (vf 0), create the script object, call its vf 0x10 | high | D |
| 0x4C5240 / 0x4C51D0 | ScriptStdScreen_Close / ScriptPauseScreen_Close | vf 3: release the script instance (0x7A5120); the pause screen first calls 0x59BB70(1) | medium | D |
| 0x4BDCC0 / 0x4BDD00 / 0x4BDCB0 / 0x4BDCE0 / 0x77D9B0 | ScriptMainMenu_GetType / ScriptPauseScreen_GetType / ScriptLoadScreen_GetType / ScriptOptionsScreen_GetType / ScriptHUDMenu_GetType | vf 0: global data +0xE8 / +0xE0 / +0x08 / +0xE4 / +0x198 → 0x551560 | high | V (4BDD00) |
| 0x77E9E0 / 0x77F390 | HUD_Create / HUD_Destroy | the `ScriptHUDMenu` (8 bytes) at 0xFAE920 | high | D |
| 0x5AD5A2 (in 0x5AD1B0) / 0x5AEA7C | UIWidgetBase ctors | store vftable 0x851FD4 | high | V (store) |
| 0x59C390 | UIWidget_SetParent | slot 1: +0x10 parent; the old parent's vf 0xC (remove child) | high | D |
| 0x5A1280 | UIWidget_GetAlpha | slot 26: style (+0x1F0) +0x1C × the parent's alpha | high | D |
| 0x59C750 / 0x59C790 / 0x59C7D0 | UIWidget_DrawLayerA / B / C | slots 34-36: if in that layer (slots 37-39) and alpha > 0, Draw (slot 33) | high | V (59C750) |
| 0x5A2600 / 0x5A2620 / 0x5A2640 | UIWidget_InLayerA / B / C | `(desc(+0x20)->flags(+4) & 0x180) == 0x80 / 0 / 0x100` | high | V (5A2600) |
| 0x6457A0 | (empty stub) | slot 33 Draw in the base class: every concrete widget overrides it | high | D |
| 0x5A7E40 | UIText_Draw | slot 33 of `cdc::UIText` | medium | D |
| 0x5A7970..0x5A7A50 | UISuperWidget_* | slots 33-36: walk the children list +0x224 (end sentinel 0xCDCC20) calling the child's matching slot | high | D |
| 0x6E5990 / 0x6E58C0 | UIPDAMapWidget_Draw / _DrawAnyLayer | slot 33 / slots 34-36 (draws in every layer) | medium | D |
| 0x6DF600 | PDAMap_LoadDrm | "PDA MAP DRM %s Loaded / Has %i faces …"; adds the map mesh to the multibody system | high | D (strings) |
| 0x6E4410 | PDAMap_Deserialize | ">>>>> PDA MAP DATA IS BEING DESERIALIZED <<<<<", box / mesh counts | high | D (strings) |
| 0x6E4800 | PDAMap_Save | "PDA Map failed to save - it tried to write past the end of the save data block" (ERROR_Fatal) | high | D (strings) |
| 0x6E5650 / 0x6E56D0 | PDAMapSection_Alloc / _Free | DRM section type 13 (registered by MainG2_InitEngine) | high | D (strings) |
| 0x5B04B0 | UIScreen_GenId | `genUIScreenId_%08x` | medium | D (string) |

Existing names kept: Font_* 0x474C90..0x476BC0, Font_Flush 0x5C5A30, Text_DrawKeyHighlight 0x476D80,
GAMELOOP_SetScreenWipe / IsWipeDone 0x52E8B0 / 0x52E880.

## 3. Front-end state machine (main state 10, 0x5DAE20)

| Sub-state (0xFAE8DA) | Name (0x77CB20) | Behaviour (D) |
|---|---|---|
| 0 | LegalScreens | — |
| 1 | IntroMovie | 0x77BBC0(2) → press-start |
| 2 / 3 | PressStartScreen / MainMenuScreen | create the std screen if missing (0x4BDD90); loop `medsea_loop` (Bink 0x4A12B0) while the screen lives and its state (0x77BBE0) ≠ 4; if the movie fails, a plain platform loop (MainG2_PlatformUpdate, STREAM_PollLoadQueue, draw) |
| 4 | StartingNewGame | — |
| 5 | PlayExtrasMovie | plays the movie named at 0xE7ECE0 |
| 6 / 7 | StartingAttractMode / InAttractMode | — |
| 8 | PlayNewGameMovie | plays `explode.bik`, then chapter 0 (0x5DB110; GAMEPLAY.md:4452) |

Main states (`mainTracker` 0xE7ED60, 0x5E0090, D except case 8): 1 play game, 2 level load (0x5DE7E0), 3 → 6, 5
(0x5D5520), 6 restart (0x555220 + 0x5DAAB0), 7 (0x5D5680), **8 quit**, 9 paused draw-only frame, 10 front end.
Exit to the OS: the script native `uigamedata` 45 sets state 8 → the next MainG2 iteration ends the loop
([SHUTDOWN.md](SHUTDOWN.md)).

## 4. Widget model

Class family (class_hierarchy.tsv, 19 rows under `cdc::UIWidgetBase`): `UIBitmap`, `UIDynamicBitmap`, `UIButton`,
`UIText`, `UITextEntry`, `UIDebugText`, `UIMenuWdgt`, `UIMenuEntry` → `UIMenuBoolItem` / `UIMenuEnumItem`, `UIMeter`,
`UIModel` (3-D models in the PDA), `UINGAPoly`, `UIRenderImg`, `UIScrollbar`, `UISlider`, `UIPDAMapWidget`,
`UISuperWidget` → `ScriptUIScreen`. Properties: `UIPropertyBase<T>` for bool / int / float / Vector2-4 / ScriptSymbol
(and Bool, Color, Float, Int, LocalStr, String, Vec2, Vec3 subclasses, 10 slots). Animation: `UITimeline<T>` (int,
float, Vector2-4) and `UIAnimation`.

`cdc::UIWidgetBase` (vftable 0x851FD4, 63 slots), fields read so far:

| Offset | Meaning | Evidence |
|---|---|---|
| +0x10 | parent widget | 0x59C390, 0x5A1280 |
| +0x14 | owner / screen (its +4 byte gates child drawing) | 0x5A7A10 |
| +0x20 | descriptor; +4 flags, bits 7-8 = draw layer | 0x5A2600 (V) |
| +0x1F0 | current style; +0x1C alpha | 0x59C761 (V), 0x5A1280 |
| +0x224 | (UISuperWidget) children list | 0x5A7A10 |

Draw: a screen draws three passes; each widget draws in exactly one (layer A = flags 0x80, B = 0, C = 0x100) when its
hierarchical alpha is > 0 (V). The PDA map widget draws in all three.

## 5. HUD and PDA

- HUD: a `ScriptHUDMenu` std screen whose script type is global data +0x198 (D); the HUD elements are widgets of that
  script (meters, bitmaps, text) [I, high: no native HUD drawing found; `NtHUDScreenMgr` is the script-side manager].
- PDA: script screens plus the native `cdc::UIPDAMapWidget` (sonar map) fed by the PDA map DRMs (section type 13,
  `PDAMapSection` 0x8552BC) and reveal shapes `PDAMap::RevealedInfo_{Box,Capsule,Sphere}`; the revealed state is
  serialised into the save buffer (0x6E4800) [I, medium: which save block — not traced].
- Eight script-object singletons at 0xFAE8E4, 0xFAE8EC, 0xFAE8F8, 0xFAE90C, 0xFAE91C, 0xFAE92C, 0xFAE930, 0xFAE934 are
  created by 0x77E5A0.. (holders 0x77DDF0 / 0x77DE80 / 0x77DF60 / 0x77E4A0, script create 0x4A2970), read by
  getters 0x77D900..0x77DB30 and released at shutdown (0x77FA40..0x77FB90) [U: their script types].

## 6. Differences vs OpenUnderworld

| # | tru.exe | OpenUnderworld | Evidence |
|---|---|---|---|
| 1 | Menus, HUD and PDA pages are script objects (NsUIScreen) over cdc widget trees; layouts and behaviour are data | Native C++ screens (`ui::Screen` enum, 32 screens) at layouts measured from captures; "the original UI scripts are not executed" | src/ui/Ui.hpp:1-5, :83-86 |
| 2 | Three draw layers per widget, hierarchical alpha | One quad list in draw order (`render::TextVertex`) | src/ui/Ui.hpp:1-3 |
| 3 | Front end: legal screens → intro movie → press start → main menu over the looping Bink `medsea_loop`; attract mode (sub-states 6 / 7); New Game plays `explode.bik` (sub-state 8, 0x5DB110) | The main menu loops `medsea_loop.bik` (src/app/game/main.cpp:1962, :2337); no legal screens, press-start screen or attract mode found in src | src/app/game/main.cpp; GAMEPLAY.md:4452 |
| 4 | Quit = script native `uigamedata` 45 → main state 8 → MainG2 exits → 0x5DF930 | Action::Quit in the native menu → app exit | src/ui/Ui.cpp:535 |
| 5 | PDA sonar map drawn by the renderer from PDA map DRMs (UIPDAMapWidget) | CPU-rasterised images handed to GL/VK | src/ui/Pda.hpp:1-5 |
| 6 | HUD = ScriptHUDMenu widgets | Native `ui::Hud` from generalbank art | src/ui/Hud.cpp:1-3 |

These are replacements by design (no script UI runtime); none is a bug by itself. Fidelity risks: menu timelines
(fades, slide-ins) and attract mode are not reproduced.

## 7. Unknowns

- The other 59 UIWidgetBase slots (input, focus, property access, timeline update) are unnamed.
- Which global-data entries name the save / PDA / tailoring screens (only +0x08, +0xE0, +0xE4, +0xE8, +0x198 seen).
- The types of the eight singletons in §5 and the screen state values 0x77BBE0 returns (4 = done).
