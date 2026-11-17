#!/usr/bin/env python3
"""OpenUnderworld fixed benchmark scenarios (docs/audit/BASELINE_PERF_2026-11.md).

Runs ou-game on fixed levels with an --input-script, a fixed --render-hz and --frames, so every run simulates the
same ticks (the `sim` digest must match between runs). Per run it records: frame/tick timings (the game's own report
and the per-frame dump from OU_FRAME_TIMES), level-load and startup time, cold vs warm shader/pipeline caches, RSS,
per-process VRAM and GPU engine time (amdgpu DRM fdinfo), device VRAM and gpu_busy_percent (amdgpu sysfs) and the
process CPU %. Output: <out>/bench.csv plus per-run logs. Run it inside a Wayland session (tools/bench/run.sh starts
a private KWin); `--compare` checks a finished CSV for reproducibility.

Needs only the Python standard library.
"""
import argparse
import csv
import glob
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FRAMES = 900          # 15 s of simulated time at 60 Hz
RENDER_HZ = 60
WARMUP = 30           # ou-game drops the first 30 frames from its report; the frame dump marks them
HIST_EDGES = [2, 4, 8, 16.7, 33.3]  # ms bucket edges for the frame-pacing histogram

BED = r"pc-w\l3_main_bedroom.drm"
CLIFF = r"pc-w\l2_cliffclimb.drm"
JUNGLE = r"pc-w\l2_junglehub.drm"
SEAFLOOR = r"pc-w\l1_seafloor.drm"

# name, renderer, level, input script, extra ou-game flags. Every flag that affects the look is explicit, so a later
# change of the defaults does not silently change a scenario.
COMMON_GFX = ["--preset", "enhanced", "--shaders", "original", "--rt", "auto", "--upscaler", "off",
              "--frame-generation", "off", "--anti-lag", "off", "--msaa", "4"]
SCENARIOS = [
    ("bedroom_vk_ao_off",    "vulkan", BED,      "bedroom.txt",   ["--ao", "off"]),
    ("bedroom_vk_ao_cortao", "vulkan", BED,      "bedroom.txt",   ["--ao", "cortao"]),
    ("bedroom_vk_ao_rt",     "vulkan", BED,      "bedroom.txt",   ["--ao", "rt"]),
    ("bedroom_gl_ao_cortao", "opengl", BED,      "bedroom.txt",   ["--ao", "cortao"]),
    ("cliff_vk_water_enh",   "vulkan", CLIFF,    "cliff_sea.txt", ["--ao", "cortao", "--water", "enhanced"]),
    ("cliff_vk_water_orig",  "vulkan", CLIFF,    "cliff_sea.txt", ["--ao", "cortao", "--water", "original"]),
    ("jungle_vk",            "vulkan", JUNGLE,   "jungle.txt",    ["--ao", "cortao"]),
    ("seafloor_vk_water_enh", "vulkan", SEAFLOOR, "seafloor.txt", ["--ao", "cortao", "--water", "enhanced"]),
]

FIELDS = ["run", "scenario", "cache", "renderer", "rc", "final_tick", "sim", "pos",
          "load_s", "loop_start_ms", "first3_frames_ms",
          "frame_mean_ms", "frame_p50_ms", "frame_p95_ms", "frame_p99_ms", "frame_max_ms", "frames",
          "hist_lt2", "hist_2_4", "hist_4_8", "hist_8_16.7", "hist_16.7_33.3", "hist_gt33.3",
          "tick_mean_ms", "tick_p95_ms", "script_ms", "physics_ms", "player_ms",
          "cpu_pct", "rss_run_mb", "rss_peak_mb", "vram_proc_mb", "vram_dev_delta_mb", "gpu_busy_pct", "gpu_gfx_pct",
          "wall_s", "ao_rt_line"]


def find_amdgpu():
    for dev in sorted(glob.glob("/sys/class/drm/card*/device")):
        if os.path.exists(f"{dev}/gpu_busy_percent") and os.path.exists(f"{dev}/mem_info_vram_used"):
            return dev
    return None


def read_int(path):
    try:
        with open(path) as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def proc_status(pid):
    out = {}
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                k, _, v = line.partition(":")
                if k in ("VmRSS", "VmHWM"):
                    out[k] = int(v.split()[0])  # kB
    except OSError:
        pass
    return out


def proc_cpu_s(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")  # utime + stime
    except (OSError, IndexError, ValueError):
        return None


def proc_drm(pid):
    """Sum per DRM client (deduplicated by drm-client-id) of VRAM (KiB) and gfx engine time (ns) from fdinfo."""
    clients = {}
    for fd in glob.glob(f"/proc/{pid}/fdinfo/*"):
        try:
            with open(fd) as f:
                text = f.read()
        except OSError:
            continue
        if "drm-client-id" not in text:
            continue
        info = dict(line.split(":", 1) for line in text.splitlines() if ":" in line)
        cid = info.get("drm-client-id", "").strip()
        vram = info.get("drm-memory-vram", "0 KiB").split()
        gfx = info.get("drm-engine-gfx", "0 ns").split()
        kib = int(vram[0]) * {"KiB": 1, "MiB": 1024, "GiB": 1 << 20}.get(vram[1] if len(vram) > 1 else "KiB", 1)
        clients[cid] = (kib, int(gfx[0]))
    if not clients:
        return None
    return sum(v[0] for v in clients.values()), sum(v[1] for v in clients.values())


class Sampler(threading.Thread):
    def __init__(self, pid, gpu, period=0.1):
        super().__init__(daemon=True)
        self.pid, self.gpu, self.period = pid, gpu, period
        self.samples = []  # (t, rss kB, hwm kB, cpu s, drm (vram KiB, gfx ns) | None, busy %, dev vram bytes)
        self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            st = proc_status(self.pid)
            if not st:
                break
            self.samples.append((time.monotonic(), st.get("VmRSS"), st.get("VmHWM"), proc_cpu_s(self.pid),
                                 proc_drm(self.pid),
                                 read_int(f"{self.gpu}/gpu_busy_percent") if self.gpu else None,
                                 read_int(f"{self.gpu}/mem_info_vram_used") if self.gpu else None))
            self.stop.wait(self.period)


def pct(sorted_v, p):
    return sorted_v[min(len(sorted_v) - 1, int(len(sorted_v) * p / 100))]


def parse_report(log):
    r = {}
    m = re.search(r"final tick (\d+) .*? pos ([-\d.]+ [-\d.]+ [-\d.]+) .*? sim ([0-9a-f]{16})", log)
    if m:
        r.update(final_tick=int(m.group(1)), pos=m.group(2), sim=m.group(3))
    m = re.search(r"frame time mean ([\d.]+) ms p95 ([\d.]+) ms max ([\d.]+) ms", log)
    if m:
        r.update(report_frame_mean=float(m.group(1)))
    m = re.search(r"tick time mean ([\d.]+) ms median [\d.]+ ms p95 ([\d.]+) ms, script ([\d.]+) ms, physics ([\d.]+) ms, "
                  r"player ([\d.]+) ms", log)
    if m:
        r.update(tick_mean_ms=float(m.group(1)), tick_p95_ms=float(m.group(2)), script_ms=float(m.group(3)),
                 physics_ms=float(m.group(4)), player_ms=float(m.group(5)))
    m = re.search(r"^ou-game: (AO .*)$", log, re.M)
    if m:
        r["ao_rt_line"] = m.group(1)
    return r


def parse_frames(path):
    frames, loop_start = [], None
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("# loop_start_ms"):
                    loop_start = float(line.split()[2])
                elif line.startswith("frame,"):
                    frames.append(float(line.rsplit(",", 1)[1]))
    except OSError:
        return {}
    r = {"loop_start_ms": loop_start}
    if len(frames) > 3:
        r["first3_frames_ms"] = round(sum(frames[1:4]), 3)  # frames[0] is loop start -> first iteration (no draw)
    run = sorted(frames[WARMUP + 1:])  # same window as the game's report (frame > 30)
    if run:
        r.update(frame_mean_ms=statistics.fmean(run), frame_p50_ms=pct(run, 50), frame_p95_ms=pct(run, 95),
                 frame_p99_ms=pct(run, 99), frame_max_ms=run[-1], frames=len(run))
        edges = [0] + HIST_EDGES + [float("inf")]
        names = ["hist_lt2", "hist_2_4", "hist_4_8", "hist_8_16.7", "hist_16.7_33.3", "hist_gt33.3"]
        for name, lo, hi in zip(names, edges, edges[1:]):
            r[name] = sum(1 for t in run if lo <= t < hi)
    return r


def run_one(args, run, name, renderer, level, script, extra, cache, cache_dir, out):
    rdir = out / f"run{run}" / f"{name}_{cache}"
    rdir.mkdir(parents=True, exist_ok=True)
    xdg = out / "xdg"
    env = dict(os.environ)
    for k, sub in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("XDG_STATE_HOME", "state")):
        env[k] = str(xdg / sub)
        (xdg / sub).mkdir(parents=True, exist_ok=True)
    if cache == "cold" and cache_dir.exists():
        shutil.rmtree(cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)
    env["XDG_CACHE_HOME"] = str(cache_dir)  # vk_pipeline_cache.bin and Mesa's own shader cache both live here
    env["MESA_SHADER_CACHE_DIR"] = str(cache_dir)
    env["OU_FRAME_TIMES"] = str(rdir / "frames.csv")
    env["OU_GAME_DIR"] = args.game_dir
    env["OU_DISABLE_UPDATES"] = "1"
    cmd = [args.ou_game, "--game-dir", args.game_dir, "--renderer", renderer, "--level", level,
           "--input-script", str(HERE / "scripts" / script), "--render-hz", str(RENDER_HZ), "--frames", str(FRAMES),
           "--width", "1920", "--height", "1080", "--no-vsync", "--no-overlay", "--no-audio", "--no-movies",
           "--no-cinematics", "--sync-loads", "--screenshot", str(rdir / "final.ppm")] + COMMON_GFX + extra
    (rdir / "cmd.txt").write_text(" ".join(cmd) + "\n")
    gpu = find_amdgpu()
    dev_vram0 = read_int(f"{gpu}/mem_info_vram_used") if gpu else None
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, text=True, errors="replace")
    sampler = Sampler(proc.pid, gpu)
    sampler.start()
    lines, t_loaded = [], None
    with open(rdir / "stderr.log", "w") as log:
        for line in proc.stderr:
            t = time.monotonic() - t0
            log.write(f"{t:9.3f} {line}")
            lines.append(line)
            if t_loaded is None and re.match(r"ou-game: tick 0 in unit", line):
                t_loaded = t
    rc = proc.wait()
    wall = time.monotonic() - t0
    sampler.stop.set()
    sampler.join()
    text = "".join(lines)
    row = {"run": run, "scenario": name, "cache": cache, "renderer": renderer, "rc": rc, "wall_s": round(wall, 3),
           "load_s": round(t_loaded, 3) if t_loaded is not None else ""}
    rep = parse_report(text)
    rep.pop("report_frame_mean", None)
    row.update(rep)
    row.update(parse_frames(rdir / "frames.csv"))
    # Run phase: from the level-loaded line to the last sample (the process is gone after that).
    s = sampler.samples
    run_s = [x for x in s if t_loaded is not None and x[0] - t0 >= t_loaded + 1.0] or s
    if len(run_s) >= 2:
        a, b = run_s[0], run_s[-1]
        dt = b[0] - a[0]
        if dt > 0 and a[3] is not None and b[3] is not None:
            row["cpu_pct"] = round(100 * (b[3] - a[3]) / dt, 1)
        drm = [x for x in run_s if x[4]]  # fdinfo can be unreadable for a sample (fd churn, exit)
        if len(drm) >= 2 and drm[-1][0] > drm[0][0] and drm[-1][4][1] >= drm[0][4][1]:
            row["gpu_gfx_pct"] = round(100 * (drm[-1][4][1] - drm[0][4][1]) * 1e-9 / (drm[-1][0] - drm[0][0]), 1)
        busy = [x[5] for x in run_s if x[5] is not None]
        if busy:
            row["gpu_busy_pct"] = round(statistics.fmean(busy), 1)
        row["rss_run_mb"] = round(statistics.median(x[1] for x in run_s if x[1]) / 1024, 1)
    if s:
        row["rss_peak_mb"] = round(max(x[2] or 0 for x in s) / 1024, 1)
        vram = [x[4][0] for x in s if x[4]]
        if vram:
            row["vram_proc_mb"] = round(max(vram) / 1024, 1)
        dev = [x[6] for x in s if x[6] is not None]
        if dev and dev_vram0 is not None:
            row["vram_dev_delta_mb"] = round((max(dev) - dev_vram0) / (1 << 20), 1)
    for k in ("frame_mean_ms", "frame_p50_ms", "frame_p95_ms", "frame_p99_ms", "frame_max_ms"):
        if k in row:
            row[k] = round(row[k], 3)
    (rdir / "samples.json").write_text(json.dumps(
        {"t0": t0, "dev_vram0": dev_vram0, "gpu": gpu, "samples": s}, indent=None))
    return row


def compare(csv_path, noise_pct):
    rows = list(csv.DictReader(open(csv_path)))
    groups = {}
    for r in rows:
        groups.setdefault((r["scenario"], r["cache"]), []).append(r)
    ok = True
    print(f"{'scenario':24} {'cache':5} {'runs':>4} {'sim':>8} {'mean ms':>16} {'p95 ms':>16} {'load s':>14}  spread")
    for (sc, cache), g in sorted(groups.items()):
        sims = {r["sim"] for r in g}
        ticks = {r["final_tick"] for r in g}
        same = len(sims) == 1 and len(ticks) == 1 and all(r["rc"] == "0" for r in g)
        ok &= same

        def span(k):
            v = [float(r[k]) for r in g if r.get(k)]
            if not v:
                return "-", 0.0
            mid = statistics.fmean(v)
            return "/".join(f"{x:.3f}" for x in v), (100 * (max(v) - min(v)) / mid if mid else 0.0)
        m, sm = span("frame_mean_ms")
        p, sp = span("frame_p95_ms")
        ld, _ = span("load_s")
        flag = "" if sm <= noise_pct else f"  mean spread {sm:.1f}% > {noise_pct}%"
        print(f"{sc:24} {cache:5} {len(g):4} {'same' if same else 'DIFFER':>8} {m:>16} {p:>16} {ld:>14}  "
              f"{sm:.1f}%/{sp:.1f}%{flag}")
    print("digests identical across runs" if ok else "DIGEST/TICK MISMATCH or failed run")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ou-game", default=str(HERE.parent.parent / "build-Bench" / "ou-game"))
    ap.add_argument("--game-dir", default=os.environ.get("OU_GAME_DIR", "/mnt/1st4TB/Tomb Raider Underworld"))
    ap.add_argument("--out", type=Path, default=Path("bench-out"))
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--only", default="", help="comma-separated scenario names")
    ap.add_argument("--no-cold", action="store_true", help="skip the cold-cache pass (warm only)")
    ap.add_argument("--compare", type=Path, help="only check an existing bench.csv")
    ap.add_argument("--noise", type=float, default=5.0, help="allowed frame-mean spread in %% for --compare")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for s in SCENARIOS:
            print(s[0], s[1], s[2], s[3], " ".join(s[4]))
        return 0
    if a.compare:
        return compare(a.compare, a.noise)
    if not os.environ.get("WAYLAND_DISPLAY") and not os.environ.get("DISPLAY"):
        sys.exit("bench.py: no WAYLAND_DISPLAY/DISPLAY; use tools/bench/run.sh for a private KWin session")
    only = set(filter(None, a.only.split(",")))
    sel = [s for s in SCENARIOS if not only or s[0] in only]
    a.out.mkdir(parents=True, exist_ok=True)
    csv_path = a.out / "bench.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
        w.writeheader()
        for run in range(1, a.runs + 1):
            for name, renderer, level, script, extra in sel:
                cache_dir = a.out / "cache" / name
                for cache in ([] if a.no_cold else ["cold"]) + ["warm"]:
                    row = run_one(a, run, name, renderer, level, script, extra, cache, cache_dir, a.out)
                    w.writerow(row)
                    f.flush()
                    print(f"run {run} {name:24} {cache:4} rc {row['rc']} sim {row.get('sim', '?')} "
                          f"tick {row.get('final_tick', '?')} mean {row.get('frame_mean_ms', '?')} ms "
                          f"load {row.get('load_s', '?')} s", flush=True)
    print(f"wrote {csv_path}")
    return compare(csv_path, a.noise)


if __name__ == "__main__":
    sys.exit(main())
