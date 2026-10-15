"""Measure how hard the opponents are, as bouts won and lost.

A bout is one fight in the gym. It starts when the opponent arrives. It stops when
one of the two ships dies, or when it lasts too long. Then it is a draw. The game
prints one 'bout' line for each bout. This script runs many fights at the same
time, reads those lines, and gives a table.

The script has two commands.

run: fly a matrix of headless gym fights. The seat is the player's ship. The
game flies it. Each cell of the matrix is one seat class, one opponent class and
one pilot level. Each cell is flown once for each seed.

    python tools/pilot_eval.py run
    python tools/pilot_eval.py run --levels 4 --enemies 3 --seeds 1-4 --minutes 2
    python tools/pilot_eval.py run --enemy net --name netcheck

score: count kills and deaths in a person's flying. No new run of the game is
necessary. The command reads two types of file:

- A recording from --dump (.obs). It holds the hull of the two ships, so it
  shows who won each fight. The count is approximate.
- A log of the game (any other name): the output of the game, sent to a file.
  The count of its 'bout' lines is exact.

    python tools/pilot_eval.py score captures/m1_aegis_jrn_1.obs
    python tools/pilot_eval.py score captures/m1_aegis_pro.log

All the numbers are from the side of the opponent. 'win' is the share of the
decided bouts that the opponent won. A draw is not a decided bout.

Each run must fly all of its frames. The game prints the frame count when it
stops. If a run flew fewer frames than the script asked for, the script reports
the run as short and does not use it.
"""
import argparse
import json
import math
import os
import re
import statistics
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "host", "build", "phantom.exe")
OUT_ROOT = os.path.join(ROOT, "captures", "ai_eval")

CLASSES = ["AEGIS", "LANCE", "CHARIOT", "BALLISTA"]
LEVELS = ["RAW", "JRN", "PRO", "VET", "ACE"]

# The share of decided bouts that the opponent must win against a person. The
# author set these on 2026-10-14. They apply to a person in the seat, not to the
# scripted seat. The script prints them only as a reference.
TARGET = {"RAW": (10, 20), "JRN": (25, 35), "PRO": (40, 50), "VET": (55, 65),
          "ACE": (65, 75)}

BOUT = re.compile(r"^bout (won|lost|draw) t=([\d.]+) by=(\w+) seat=(\w+) "
                  r"hull0=([\d.]+) hull=([\d.]+) enemy=(\w+) pilot=([\w-]+) "
                  r"ehull=([\d.]+)", re.M)
FRAMES = re.compile(r"^frames (\d+)", re.M)
OPEN = re.compile(r"^bout open t=([\d.]+)", re.M)
NET = re.compile(r"^ai (\w+)\s+frames (\d+).*?NET=(\d+)%", re.M)

# Where the hulls sit in an observation. These must agree with the enum in
# src/vg/vg_bot.h.
OBS_TGT_HULL = 7
OBS_OWN_HULL = 11


def span(text, top):
    """Read '1-4', '0,2,3' or '2' and give a list of whole numbers."""
    out = []
    for part in str(text).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    for v in out:
        if top is not None and not 0 <= v < top:
            raise ValueError("%d is out of range 0-%d" % (v, top - 1))
    return out


def wilson(k, n, z=1.96):
    """The 95% interval of a share k/n, as (low, high) in percent."""
    if n == 0:
        return (0.0, 100.0)
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (100.0 * max(0.0, c - h), 100.0 * min(1.0, c + h))


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

def fly(job):
    """Fly one fight. Give the job back with the output of the game added."""
    path = job["file"]
    if job["reuse"] and os.path.exists(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            job["text"] = f.read()
        return job
    cmd = [job["exe"], "--headless", "--gym", str(job["seat"]), str(job["enemy"]),
           "--bot", "--pilot", str(job["level"]), "--seed", str(job["seed"]),
           "--frames", str(job["frames"]), "--bouts", str(job["bout"])]
    cmd += job["flags"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    job["text"] = r.stdout + r.stderr
    with open(path, "w", encoding="utf-8") as f:
        f.write(" ".join(cmd) + "\n")
        f.write(job["text"])
    return job


def parse(job):
    """Read the bouts and the frame count from the output of one fight."""
    t = job["text"]
    m = FRAMES.search(t)
    job["flown"] = int(m.group(1)) if m else 0
    job["bouts"] = [{"out": b[0], "t": float(b[1]), "by": b[2],
                     "hull0": float(b[4]), "hull": float(b[5]), "ehull": float(b[8])}
                    for b in BOUT.findall(t)]
    m = OPEN.search(t)
    job["open"] = float(m.group(1)) if m else 0.0
    net = [(int(f), int(p)) for c, f, p in NET.findall(t) if c == CLASSES[job["enemy"]]]
    job["net"] = net[0][1] if net else 0
    return job


def cell_stats(jobs):
    """Add up the bouts of all the seeds of one cell."""
    bouts = [b for j in jobs for b in j["bouts"]]
    # The seat lost = the opponent won.
    won = sum(1 for b in bouts if b["out"] == "lost")
    lost = sum(1 for b in bouts if b["out"] == "won")
    draw = sum(1 for b in bouts if b["out"] == "draw")
    dec = won + lost
    lo, hi = wilson(won, dec)
    ttk = [b["t"] for b in bouts if b["out"] != "draw"]
    # Hull taken, as a fraction of a whole ship, on each side.
    took = sum(b["hull0"] - b["hull"] for b in bouts)
    gave = sum(1.0 - b["ehull"] for b in bouts)
    by = {}
    for b in bouts:
        if b["out"] == "lost":
            by[b["by"]] = by.get(b["by"], 0) + 1
    return {
        "bouts": len(bouts), "win": won, "loss": lost, "draw": draw,
        "win_pct": 100.0 * won / dec if dec else None,
        "win_lo": lo, "win_hi": hi,
        "draw_pct": 100.0 * draw / len(bouts) if bouts else None,
        "ttk_median": statistics.median(ttk) if ttk else None,
        "exchange": took / gave if gave > 0 else None,
        "kills_by": by,
        "net_pct": statistics.mean(j["net"] for j in jobs) if jobs else 0,
        "minutes": sum(j["flown"] for j in jobs) / 3600.0,
    }


def fmt(v, spec, none="   -"):
    return none if v is None else spec % v


def report(cells, args):
    """Print the tables. Give the level summary for the JSON file."""
    seats = span(args.seats, 4)
    enemies = span(args.enemies, 4)
    levels = span(args.levels, 5)
    print("")
    print("Opponent win share of decided bouts, in percent. One column for each seat.")
    print("Last columns: all seats together, its 95% interval, draws, median bout,")
    print("and the exchange (seat hull lost / opponent hull lost).")
    print("")
    head = "%-9s %-4s " % ("opponent", "lvl")
    head += "".join("%9s" % CLASSES[s][:8] for s in seats)
    head += "  %6s %11s %6s %7s %6s %5s" % ("all", "95%", "draw", "median", "exch", "net")
    print(head)
    summary = {}
    for e in enemies:
        for lv in levels:
            row = "%-9s %-4s " % (CLASSES[e], LEVELS[lv])
            for s in seats:
                c = cells[(s, e, lv)]
                row += "%9s" % fmt(c["win_pct"], "%.0f")
            allc = cell_stats([j for s in seats for j in cells[(s, e, lv)]["jobs"]])
            row += "  %6s %11s %6s %7s %6s %5s" % (
                fmt(allc["win_pct"], "%.0f"),
                "%3.0f-%-3.0f" % (allc["win_lo"], allc["win_hi"]),
                fmt(allc["draw_pct"], "%.0f"),
                fmt(allc["ttk_median"], "%.0fs"),
                fmt(allc["exchange"], "%.2f"),
                "%.0f%%" % allc["net_pct"])
            print(row)
        print("")
    print("Each level, all seats and all opponents together:")
    print("%-4s %6s %11s %6s %7s %6s %7s   %s" % (
        "lvl", "win", "95%", "draw", "median", "exch", "bouts", "target against a person"))
    for lv in levels:
        allc = cell_stats([j for s in seats for e in enemies
                           for j in cells[(s, e, lv)]["jobs"]])
        summary[LEVELS[lv]] = {k: v for k, v in allc.items()}
        t = TARGET[LEVELS[lv]]
        print("%-4s %6s %11s %6s %7s %6s %7d   %d-%d%%" % (
            LEVELS[lv], fmt(allc["win_pct"], "%.1f"),
            "%3.0f-%-3.0f" % (allc["win_lo"], allc["win_hi"]),
            fmt(allc["draw_pct"], "%.0f"), fmt(allc["ttk_median"], "%.0fs"),
            fmt(allc["exchange"], "%.2f"), allc["bouts"], t[0], t[1]))
    by = {}
    for c in cells.values():
        for k, v in c["kills_by"].items():
            by[k] = by.get(k, 0) + v
    if by:
        print("")
        print("How the opponents killed the seat: " +
              ", ".join("%s %d" % (k, v) for k, v in sorted(by.items())))
    return summary


def cmd_run(args):
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print("error: %s does not exist. Build the PC target first." % exe)
        return 1
    seats, enemies = span(args.seats, 4), span(args.enemies, 4)
    levels, seeds = span(args.levels, 5), span(args.seeds, None)
    frames = int(round(args.minutes * 3600))
    flags = ["--scripted"] if args.seat == "scripted" else []
    flags += ["--enemy-net"] if args.enemy == "net" else ["--no-enemy-net"]
    flags += args.extra
    name = args.name or time.strftime("run-%Y%m%d-%H%M%S")
    out = os.path.join(OUT_ROOT, name)
    os.makedirs(out, exist_ok=True)

    jobs = []
    for s in seats:
        for e in enemies:
            for lv in levels:
                for sd in seeds:
                    f = os.path.join(out, "s%d_e%d_L%d_seed%d.txt" % (s, e, lv, sd))
                    jobs.append({"exe": exe, "seat": s, "enemy": e, "level": lv,
                                 "seed": sd, "frames": frames, "bout": args.bout,
                                 "flags": flags, "file": f, "reuse": args.reuse})
    print("%d fights, %g simulated minutes each, %d at a time. Output: %s"
          % (len(jobs), args.minutes, args.workers, out))
    t0 = time.time()
    done = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, j in enumerate(pool.map(fly, jobs)):
            done.append(parse(j))
            if (i + 1) % 50 == 0 or i + 1 == len(jobs):
                print("  %d of %d done, %.0f s" % (i + 1, len(jobs), time.time() - t0))

    short = [j for j in done if j["flown"] != frames]
    for j in short:
        print("SHORT: %s flew %d of %d frames. The script does not use it."
              % (os.path.basename(j["file"]), j["flown"], frames))
    good = [j for j in done if j["flown"] == frames]

    cells = {}
    for s in seats:
        for e in enemies:
            for lv in levels:
                js = [j for j in good if (j["seat"], j["enemy"], j["level"]) == (s, e, lv)]
                c = cell_stats(js)
                c["jobs"] = js
                cells[(s, e, lv)] = c
    print("")
    print("seat %s, opponent %s, %s, bout limit %.0f s, seeds %s, %g min each"
          % (args.seat, args.enemy, " ".join(flags), args.bout, args.seeds, args.minutes))
    summary = report(cells, args)

    result = {
        "args": vars(args), "frames": frames, "short": len(short),
        "cells": [{"seat": CLASSES[s], "enemy": CLASSES[e], "level": LEVELS[lv],
                   **{k: v for k, v in c.items() if k != "jobs"}}
                  for (s, e, lv), c in sorted(cells.items())],
        "levels": summary,
    }
    path = os.path.join(out, "result.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    print("")
    print("wrote %s" % path)
    return 1 if short else 0


# ---------------------------------------------------------------------------
# score
# ---------------------------------------------------------------------------

def read_obs(path):
    """Read one dump file. Give the observation rows and their width."""
    import numpy as np
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12:
            raise ValueError("%s is too short to hold a header" % path)
        magic, packed, act_n = struct.unpack("<4sII", head)
        if magic != b"PHOB":
            raise ValueError("%s is not a dump file" % path)
        raw = np.fromfile(f, dtype=np.float32)
    n = packed & 0xFFFF
    rows = raw.size // (n + act_n)
    return raw[: rows * (n + act_n)].reshape(rows, n + act_n)[:, :n], n


def score_file(path):
    """Count the kills and deaths in one recording.

    A death is a step UP in the seat's own hull: the gym gives a new, whole ship.
    A kill is a step up in the target's hull: a new opponent arrived. After a
    death the gym also sends a new opponent, on the same row. So a step up in the
    target's hull on the row of a death is not a kill.

    Any step up counts, however small. In a fight, a hull only goes down. A large
    limit missed a death at a high hull, for example a collision: the last row
    before the death showed a hull near 1.

    A return through the menus or the practice course also gives a whole ship and
    a new opponent, and this count shows it as a death. The recordings from August
    hold 20 of these steps, and the count cannot tell which they are. So use the
    log of the session when you have it. The game prints a 'bout' line for each
    bout in every session on the PC.
    """
    import numpy as np
    obs, _ = read_obs(path)
    own = np.diff(obs[:, OBS_OWN_HULL])
    tgt = np.diff(obs[:, OBS_TGT_HULL])
    deaths = set(np.flatnonzero(own > 0.005).tolist())
    kills = [k for k in np.flatnonzero(tgt > 0.005).tolist() if k not in deaths]
    minutes = obs.shape[0] / 3600.0
    taken = float(np.clip(-own, 0, None).sum())
    dealt = float(np.clip(-tgt, 0, None).sum())
    return {"file": os.path.basename(path), "minutes": minutes, "kills": len(kills),
            "deaths": len(deaths), "taken": taken, "dealt": dealt}


def read_text(path):
    """Read a log of the game. PowerShell 5 writes a redirect as UTF-16."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16", errors="replace")
    return raw.decode("utf-8", errors="replace")


def score_log(path):
    """Count the bouts in a log of the game. This count is exact.

    Any session on the PC prints a 'bout' line for each bout, with or without
    --bouts. With --bouts the game restarts each bout with a whole ship, so a
    step up in the hull is not always a death and a recording cannot show the
    result. Then the log is the only correct count.
    """
    text = read_text(path)
    bouts = BOUT.findall(text)
    if not bouts:
        raise ValueError("%s holds no 'bout' lines" % path)
    # The bout that was still in the air when the session stopped.
    m = OPEN.search(text)
    unfinished = float(m.group(1)) if m else 0.0
    kills = sum(1 for b in bouts if b[0] == "won")
    deaths = sum(1 for b in bouts if b[0] == "lost")
    seconds = sum(float(b[1]) for b in bouts) + unfinished
    taken = sum(float(b[4]) - float(b[5]) for b in bouts)
    dealt = sum(1.0 - float(b[8]) for b in bouts)
    return {"file": os.path.basename(path), "minutes": seconds / 60.0,
            "kills": kills, "deaths": deaths, "taken": taken, "dealt": dealt,
            "draws": sum(1 for b in bouts if b[0] == "draw")}


def cmd_score(args):
    rows = []
    for p in args.files:
        try:
            if p.lower().endswith(".obs"):
                rows.append(score_file(p))
            else:
                rows.append(score_log(p))
        except (OSError, ValueError) as e:
            print("error: %s" % e)
            return 1
    print("%-32s %6s %6s %6s %6s %6s %10s %10s" % (
        "recording", "min", "kills", "deaths", "draws", "win", "taken/min", "dealt/min"))
    for r in rows + [{"file": "ALL", "minutes": sum(r["minutes"] for r in rows),
                      "kills": sum(r["kills"] for r in rows),
                      "deaths": sum(r["deaths"] for r in rows),
                      "taken": sum(r["taken"] for r in rows),
                      "dealt": sum(r["dealt"] for r in rows),
                      "draws": sum(r.get("draws", 0) for r in rows)}]:
        dec = r["kills"] + r["deaths"]
        win = "%5.0f%%" % (100.0 * r["deaths"] / dec) if dec else "     -"
        m = max(r["minutes"], 1e-9)
        draws = "%6d" % r["draws"] if "draws" in r else "     -"
        print("%-32s %6.1f %6d %6d %6s %6s %10.2f %10.2f" % (
            r["file"][:32], r["minutes"], r["kills"], r["deaths"], draws, win,
            r["taken"] / m, r["dealt"] / m))
    print("")
    print("'win' is the share of the decided fights that the OPPONENT won.")
    print("taken and dealt are in whole ships per minute.")
    print("The count from a .obs file is approximate. The count from a log is exact.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Measure how hard the opponents are, as bouts won and lost.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="fly a matrix of headless gym fights")
    r.add_argument("--seats", default="0-3",
                   help="seat classes, 0 AEGIS 1 LANCE 2 CHARIOT 3 BALLISTA (default 0-3)")
    r.add_argument("--enemies", default="0-3", help="opponent classes (default 0-3)")
    r.add_argument("--levels", default="0-4",
                   help="pilot levels, 0 RAW 1 JRN 2 PRO 3 VET 4 ACE (default 0-4)")
    r.add_argument("--seeds", default="1-8", help="game seeds (default 1-8)")
    r.add_argument("--minutes", type=float, default=10.0,
                   help="simulated minutes for each fight (default 10)")
    r.add_argument("--bout", type=float, default=180.0,
                   help="seconds after which a bout is a draw (default 180)")
    r.add_argument("--seat", choices=["scripted", "net"], default="scripted",
                   help="what flies the seat (default scripted)")
    r.add_argument("--enemy", choices=["tactics", "net"], default="tactics",
                   help="what flies the opponent (default tactics)")
    r.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                   help="fights at the same time (default: half the processors)")
    r.add_argument("--name", default="", help="folder name under captures/ai_eval")
    r.add_argument("--exe", default=EXE, help="the game to run")
    r.add_argument("--reuse", action="store_true",
                   help="read the output of a fight that is already in the folder")
    r.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                   help="more flags for the game, for example --extra --no-ram")

    s = sub.add_parser("score", help="count kills and deaths in a person's flying")
    s.add_argument("files", nargs="+",
                   help=".obs recordings from --dump, or logs of --bouts sessions")

    args = ap.parse_args()
    if args.cmd == "run":
        return cmd_run(args)
    return cmd_score(args)


if __name__ == "__main__":
    sys.exit(main())
