#!/usr/bin/env python3
"""Generate docs/CHECKPOINTS.md: per-chapter checkpoint list and objective (help-on-demand) list (brief W4-1 / P4-1).

Input: the TSV of ou-chapter-data (tools/re/chapter_data_dump.cpp, a CMake target) run over the game files, plus
docs/matrix/NN_*.md ("Our units" lines) for the walkthrough chapter of each unit. Nothing is inferred from the
walkthrough: every row comes from the game data.

- Checkpoints: every `checkpoint` / `invis_checkpoint` intro (tru.exe 0x5D40C0 respawns Lara on them) of the units on
  each chapter's route (docs/PROGRESSION.md "Chapter routes"; ROUTES below), in route order, unit frame.
- Objectives: TRU has no objective screen; what it tells the player to do is the PDA's help on demand (FIELD
  ASSISTANCE: Hint / Answer), generalbank lines `vo\\lara\\sc_CC_NN_hint|answer` with subtitles (AUDIO.md §9.5).
  CC is the kChapters index (sc_00 = the prologue, sc_06 = the Andaman Sea ship, sc_07 = the Arctic Sea) [O: the
  subtitles' content]. "named by" = the route units whose DRM holds the condition string `hint_NN` (their trigger
  volumes hand it to the unit script).

Tests: ChaptersReal.CheckpointListMatchesTheData (this file against the data) and
ChaptersReal/CheckpointRespawn.PlacesLaraOnSolidGround/chN (each checkpoint loaded through the save / respawn path).

Usage: tools/re/chapter_data.py [--bin build/ou-chapter-data | --tsv file] [--check]
  (needs OU_GAME_DIR unless --tsv; --check: exit 1 if the committed file is out of date)
"""
import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/CHECKPOINTS.md"

# kChapters (src/game/Progress.hpp): name, route units in portal order (docs/PROGRESSION.md "Chapter routes", plus the
# cinematic units a route ends in).
ROUTES = [
    ("Prologue (burning Croft Manor)", ["l3_main_bedroom", "l3_bedroom_hall", "l3_main_hall"]),
    ("Mediterranean Sea", ["l1_seatop", "l1_seafloor", "l1_norse_hallway_a", "l1_kraken_entrance", "l1_krakenstairs_a",
                           "l1_krakenstairs_b", "l1_kraken_room", "l1_baldrsstairs_a", "l1_baldrsstairs_b", "l1_baldrs_tomb",
                           "l1_baldrs_sideroom", "l1_baldrs_hall", "l1_cineseatop", "l1_ship_exterior",
                           "l1_ship_upper_hallway_a", "l1_ship_upper_hallway_b", "l1_ship_upper_hallway",
                           "l1_ship_engine_hall", "l1_ship_natla_room", "l1_ship_lower_hallway", "l1_ship_sinking_hall",
                           "l1_cine_ship_sinking"]),
    ("Coastal Thailand", ["l2_cliffclimb", "l2_cliffhall", "l2_cliffhall_b", "l2_junglehub", "l2_hubhall", "l2_puzzleroom",
                          "l2_upperhallway", "l2_towerroom", "l2_towerconnector", "l2_norsehub", "l2_norsehall",
                          "l2_shrineroom", "l2_maproom", "l2_flushtunnel"]),
    ("Croft Manor", ["l3_caverns_start", "l3_chapel", "l3_stairhall", "l3_secretchamber", "l3_chamberrockhall",
                     "l3_main_bedroom", "l3_bedroom_hall", "l3_main_hall"]),
    ("Southern Mexico", ["l4_trailstart", "l4_trailtube", "l4_ballcourt", "l4_templepath", "l4_temple", "l4_staircase_upper",
                         "l4_staircase_xibalba", "l4_traprooms", "l4_staircase_mictlan", "l4_ragnarok_room", "l4_pool"]),
    ("Jan Mayen Island", ["l5_startsnow", "l5_valgrind_tunnel", "l5_valgrind", "l5_valaskjalf_entry", "l5_valaskjalf_entryb",
                          "l5_valaskjalf"]),
    ("Andaman Sea", ["l6_ship_exterior", "l6_ship_upper_hallway_a", "l6_ship_upper_hallway_b", "l6_ship_upper_hallway",
                     "l6_ship_engine_hall", "l6_ship_natla_room"]),
    ("Arctic Sea", ["l7_thin_ice", "l7_arctic_sea", "l7_helheim_hall01", "l7_helheim_hall02", "l7_helheim01", "l7_helheim02",
                    "l7_helheim03", "l7_yggdrasil", "l7_yggdrasil_hall01", "l7_yggdrasil_hall02", "l7_yggdrasil_end",
                    "l7_monastery"]),
    ("Epilogue", ["l7_himalaya_end"]),
]
UNIT = re.compile(r"\bl\d_[a-z0-9_]+[a-z0-9]")
# Respawn result (ChaptersReal/CheckpointRespawn): every checkpoint "stays" (60 ticks after the respawn Lara still stands
# on the floor she was born on, or swims, at the intro) except these, whose reason the test cannot see: (unit, uid) ->
# reason. The test asserts that she does not stay at them, so a fix shows up as a failure until the row is updated.
_POOL = ("dies: at a fresh load the eitr (pool box 5608) stands over the intro; the checkpoint belongs to the pool "
         "levels the section 14 / 15 scripts set (docs/matrix/15 C15-01)")
_ARENA = ("no floor under the intro at a fresh load: she drops onto a ledge {} below and hangs [U: what carries these in "
          "the original; the Natla arena's floating runestones ride orbit rigs, known gap ch21_platform_ride]")
NOT_STAYING = {
    ("l1_seatop", 1000): "stands on the yacht deck, which carries her down to 437 below the intro as it settles at the sea "
                         "level [U: the original's deck height here not measured]",
    ("l2_cliffclimb", 3833): "a small brown spider (4029) reaches and bites her (state hit, health 940 after 60 ticks)",
    ("l3_secretchamber", 4598): "a fresh load plays cinematic 145_man_powerdown, whose end places her 387 away",
    ("l4_traprooms", 5722): "a fresh load plays cinematic 195_mex_lords, whose end places her elsewhere in the room",
    ("l4_traprooms", 5733): "a fresh load plays cinematic 195_mex_lords, whose end places her elsewhere in the room",
    ("l4_traprooms", 5734): "a fresh load plays cinematic 195_mex_lords, whose end places her elsewhere in the room",
    ("l4_traprooms", 5735): "a fresh load plays cinematic 195_mex_lords, whose end places her elsewhere in the room",
    ("l4_traprooms", 5736): "a fresh load plays cinematic 195_mex_lords, whose end places her elsewhere in the room",
    ("l4_pool", 5498): _POOL, ("l4_pool", 5499): _POOL, ("l4_pool", 5500): _POOL, ("l4_pool", 5501): _POOL,
    ("l4_pool", 5503): _POOL, ("l4_pool", 5504): _POOL, ("l4_pool", 5506): _POOL, ("l4_pool", 5507): _POOL,
    ("l5_valgrind", 7329): "a fresh load plays cinematic 240_val_gate, whose end places her 107 away",
    ("l5_valaskjalf_entry", 7281): "a fresh load plays cinematic 240_val_gate, whose end places her 7120 away (in l5_valgrind)",
    ("l7_helheim03", 8844): "born on a slide face: she slides down it [U: the original not measured here]",
    ("l7_yggdrasil", 8792): "a fresh load plays cinematic 300_hel_mother, whose end places her 7351 away",
    ("l7_yggdrasil_end", 8242): _ARENA.format(416), ("l7_yggdrasil_end", 8243): _ARENA.format(525),
    ("l7_yggdrasil_end", 8244): _ARENA.format(520), ("l7_yggdrasil_end", 8245): _ARENA.format(508),
    ("l7_yggdrasil_end", 8248): "the face under the intro is 66 degrees steep (normal z 0.41): she falls 14927 [U: what carries it in the original]",
}


def read_tsv(text):
    checkpoints = defaultdict(list)  # unit -> [(uid, object, x, y, z, yaw)]
    conditions = defaultdict(set)    # unit -> {hint_NN}
    lines = {}                       # stream -> (dtp, text)
    for row in text.splitlines():
        f = row.split("\t")
        if f[0] == "checkpoint":
            checkpoints[f[1]].append((int(f[2]), f[3], f[4], f[5], f[6], f[7]))
        elif f[0] == "condition":
            conditions[f[1]].add(f[2])
        elif f[0] == "line":
            lines[f[1]] = (int(f[2]), f[3] if len(f) > 3 else "")
    return checkpoints, conditions, lines


def walkthrough_units():
    """unit -> [matrix file stem] whose "Our units" line names it."""
    out = defaultdict(list)
    for f in sorted((ROOT / "docs/matrix").glob("[0-9][0-9]_*.md")):
        for line in f.read_text().splitlines():
            if line.startswith("Our units"):
                names = set(UNIT.findall(line))
                # "l1_krakenstairs_a / _b": the second half shares the prefix
                for m in re.finditer(r"(l\d_[a-z0-9_]+?)_([a-z0-9]+) / _([a-z0-9]+)", line):
                    names.add(f"{m.group(1)}_{m.group(3)}")
                for m in re.finditer(r"(l\d_[a-z_]+?)(\d\d) / (?:l\d_[a-z_]+?)?(\d\d)\b", line):
                    names.add(f"{m.group(1)}{m.group(3)}")
                for n in names:
                    out[n].append(f.stem[:2])
                break
    return out


def generate(text):
    checkpoints, conditions, lines = read_tsv(text)
    on_route = {u for _, units in ROUTES for u in units}
    stray = sorted(u for u in checkpoints if u not in on_route)
    if stray:
        sys.exit(f"chapter_data.py: checkpoints in units on no route: {', '.join(stray)} (add them to ROUTES)")
    wt = walkthrough_units()
    total = len({(u, c[0]) for u, cs in checkpoints.items() for c in cs})
    out = [
        "# Checkpoints and objectives per chapter",
        "",
        "Generated by `tools/re/chapter_data.py` from the game files (`ou-chapter-data`, tools/re/chapter_data_dump.cpp)",
        "— do not edit by hand. Regenerate: `OU_GAME_DIR=… tools/re/chapter_data.py --bin build-X/ou-chapter-data`;",
        "`--check` fails when this file is out of date.",
        "",
        "- **Checkpoints** [O]: every `checkpoint` / `invis_checkpoint` intro of the chapter's route units",
        "  (docs/PROGRESSION.md \"Chapter routes\"), route order; position in the unit's own frame, yaw = Lara's heading",
        "  on respawn (intro rotation z − 90°, `game::Checkpoint`). tru.exe 0x5D40C0 births Lara on the stored checkpoint",
        "  id (docs/GAMEPLAY.md \"Checkpoints and respawn\"). Walkthrough = docs/matrix files whose \"Our units\" name the unit.",
        "- **Test**: each chapter's rows run in `ChaptersReal/CheckpointRespawn.PlacesLaraOnSolidGround/chN`",
        "  (ou_progress_tests), each checkpoint in a World of its own with the chapter's gear: the checkpoint goes through",
        "  a save (`save::capture` → serialize → parse) into `World::setCheckpoint` and `World::respawn` loads its unit.",
        "  Every row: Lara is born in that unit on the intro's vertical, at most 128 above it, on a walkable floor face",
        "  under her feet (or swimming). \"respawn (60 ticks)\": `stays` = 60 ticks later she is alive and still there",
        "  (standing within 64 units, or swimming); otherwise the observed reason, which the test asserts still holds.",
        "  `ChaptersReal.CheckpointListMatchesTheData` checks every row against the data and that no checkpoint is missing.",
        "- **Objectives** [O]: TRU shows no objective list; the player's goals are the PDA's FIELD ASSISTANCE lines",
        "  (help on demand, docs/formats/AUDIO.md §9.5): `vo\\lara\\sc_CC_NN_hint` / `_answer` in generalbank with",
        "  English subtitles, CC = the kChapters index (sc_00 = the prologue). \"named by\" = route units of the chapter",
        "  whose DRM holds the condition string `hint_NN` (their trigger volumes hand it to the unit script); \"—\" = no",
        "  route unit names it.",
        "",
        f"Totals: {total} checkpoints in {len(checkpoints)} units; {len(lines)} help lines.",
        "",
    ]
    for ch, (name, units) in enumerate(ROUTES):
        out += [f"## Chapter {ch}: {name}", ""]
        files = sorted({f for u in units for f in wt.get(u, [])})
        out += [f"Route units: {' → '.join(units)}. Walkthrough chapters: "
                f"{', '.join('docs/matrix/' + f + '_*.md' for f in files) if files else '—'}.", ""]
        out += ["### Checkpoints", ""]
        rows = [(u, c) for u in units for c in sorted(checkpoints.get(u, []))]
        if rows:
            out += ["| # | unit | uid | object | x | y | z | yaw | walkthrough | respawn (60 ticks) |",
                    "|---|---|---|---|---|---|---|---|---|---|"]
            for i, (u, c) in enumerate(rows, 1):
                out.append(f"| {ch}.{i} | {u} | {c[0]} | {c[1]} | {c[2]} | {c[3]} | {c[4]} | {c[5]} | "
                           f"{', '.join(wt.get(u, [])) or '—'} | {NOT_STAYING.get((u, c[0]), 'stays')} |")
            out += ["", f"Test: `ChaptersReal/CheckpointRespawn.PlacesLaraOnSolidGround/ch{ch}`."]
        else:
            out.append(f"None: the chapter's units hold no checkpoint intro; a death restarts at the start unit's "
                       f"player intro (test: CheckpointRespawn/ch{ch} asserts the empty list).")
        out += ["", "### Objectives (help on demand)", ""]
        prefix = f"vo\\lara\\sc_{ch:02d}_"
        nums = sorted({s[len(prefix):len(prefix) + 2] for s in lines if s.startswith(prefix)})
        if nums:
            out += ["| # | hint | answer | named by |", "|---|---|---|---|"]
            for n in nums:
                def text(kind):
                    line = lines.get(f"{prefix}{n}_{kind}")
                    return "(no line)" if line is None else line[1] or "(no subtitle)"
                hint, ans = text("hint"), text("answer")
                by = [u for u in units if f"hint_{n}" in conditions.get(u, set())]
                out.append(f"| {ch}.{int(n)} | {hint.replace('|', '/')} | {ans.replace('|', '/')} | {', '.join(by) or '—'} |")
        else:
            out.append("None: generalbank has no help line for this chapter.")
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=str(ROOT / "build/ou-chapter-data"))
    ap.add_argument("--tsv")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.tsv:
        text = Path(a.tsv).read_text()
    else:
        if not os.environ.get("OU_GAME_DIR"):
            sys.exit("chapter_data.py: set OU_GAME_DIR (or pass --tsv)")
        text = subprocess.run([a.bin], check=True, capture_output=True, text=True).stdout
    doc = generate(text)
    if a.check:
        if not OUT.exists() or OUT.read_text() != doc:
            print(f"{OUT.relative_to(ROOT)} is out of date; run tools/re/chapter_data.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(doc)
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
