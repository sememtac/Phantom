"""Improve a trained pilot network by evolution, in the headless game.

The network from train_pilot.py copies a person. This script makes it fight
better. Each generation it makes copies of the network with small random changes,
flies each copy as the opponent in many headless gym fights, scores the fights,
and moves the network toward the copies that scored well. This is an evolution
strategy (ES) with mirrored samples and rank-based weights.

Nothing in the game is rebuilt. Each copy is written as an int8 network file and
the game loads it with --net-file. That file is exactly what the board runs, so a
score on the PC is a score for the board's network.

    python tools/es_pilot.py --start captures/ai_eval/m2/pilot_net_mixed32F.h \\
        --name es1 --exe host/build/phantom.exe

THE SCORE (the fitness). It is computed from the side of the opponent, which is
the network. The author asked for opponents that are more aggressive, keep their
hard-to-hit movement, and pull out of a dead circle and come back in. Per minute
of flight, or as a share of frames:

    + kills    the seat died to a MISSILE: the network finished a fight
    + ttk      each such kill again, scaled by how early in the bout it came
    - deaths   the network died, by any cause
    - draws    a bout reached the time limit: a fight not finished
    - rams     a bout that ended in a collision, whoever survived. Strictly
               worse than a draw: the author rejected suicidal ramming, and with
               draws penalised a ram would otherwise be the cheapest way out
      (a seat that dies against the wall scores nothing: the network did not
       earn it, and it is not a draw either)
    + damage   seat hull taken minus network hull lost
    - hit      seat missiles that hit the network: keep the movement
    + armed    share of frames with a shot ready (lock, round, cool trigger)
    + reengage new passes: a lock taken after 2 s or more without one
    - dead     share of frames more than 4 s after the last lock: a dead circle
    + lock     share of frames with a lock: time in the firing envelope
    + guided   share of BALLISTA rounds that hit: the semi-active lock held

These parts come from the game's fight census (the `fight` and `break` lines). An
older game that does not print those lines gives them as zero. Their weights are
zero unless --weights sets them:

    - circle     share of frames in a circle: both ships turning hard, the range
                 steady, no lock (the author: a stalemate that wastes time)
    + tail       share of frames with the network's nose on the seat's tail
    + tailshots  rounds fired from the seat's tail
    + passes     extend, return with the nose on the seat, and fire
    + breakhits  hits on the seat by rounds fired on a break's run (after the
                 turn-in): what the author asked for more of
    + breakshots rounds fired on a break's run
    + expohits   hits on the seat by rounds fired at an exposed seat (the break's
                 second kind of shot; never counted as break hits)
    + exposhots  rounds fired at an exposed seat
    + hitseat    the network's missiles that hit the seat
    + evade      share of frames with one of its locked rounds 2 s or less from
                 the seat: the seat has to evade now (pressure the author feels)
    + solshots   rounds fired with a solution: a lock, the seat in reach, the seat
                 slower than the round, and not crossing faster than half its speed
    + solhits    hits on the seat by those rounds
    - wallshots  its rounds that ended in the wall
    + illumhits  hits on the seat while a semi-active hull (BALLISTA) held the
                 light on its round
    + illumhold  share of frames it held the light
    + letgo      times a minute it let go for a round judged likely to hit it

The eval table also gives the EXCHANGE: hits on the seat divided by hits on the
network. It ranked the author's own sittings in the right order, so it is the
check for pressure. It is not a part of the score.

THE RAM RULE (--ram-rule): in a bout that ends in a ram, the network gets no
credit for the damage it did. Then a ram is worse than a draw whatever the hulls,
as long as the rams weight is more than the draws weight.

Each class is scored apart and the four scores are added, so that a weak class
counts as much as a strong one. --weights changes the weights. --eval scores
networks on fixed fights and prints these parts, without training.

THE LEAGUE is what the network fights against. --seat-kinds gives the seats and
the weight of each: "scripted" (the scripted seat), "plain" (the network in
--seat-blob, the imitation of the author) and "curve" (the same network, with its
stick and throttle stretched to the author's own spread). For example
"scripted:0.5,plain:1,curve:1". Without --seat-kinds, the seat is the scripted
pilot, and each --seat-net adds a frozen network seat of weight 1.

THE ANCHOR pulls the network back toward the start a little each generation, so
that it stays close to the person it copied and does not find a way to win that
looks wrong in the game.

Each generation writes gen_NNN.h (the float network, for train_pilot.py and the
inspector) and gen_NNN.bin (the int8 file) into captures/ai_eval/es/NAME, and one
line of scores into log.jsonl.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inspect_pilot as ip     # noqa: E402
import quantize_pilot as qp    # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_ROOT = os.path.join(ROOT, "captures", "ai_eval", "es")
CLASSES = ["AEGIS", "LANCE", "CHARIOT", "BALLISTA"]

BOUT = re.compile(r"^bout (won|lost|draw) t=([\d.]+) by=(\w+) seat=(\w+) "
                  r"hull0=([\d.]+) hull=([\d.]+) enemy=(\w+) pilot=([\w-]+) "
                  r"ehull=([\d.]+)", re.M)
FRAMES = re.compile(r"^frames (\d+)", re.M)
CENSUS = re.compile(r"^ai (\w+)\s+frames (\d+) \| aim [-\d.]+ \| locked (\d+)% \| "
                    r"armed ([\d.]+)%.*?\| reengage (\d+) dead (\d+)%", re.M)
MSL = re.compile(r"^mslend them = hit (\d+) near (\d+) fuse (\d+) wall (\d+) gone (\d+)", re.M)
MSL_YOU = re.compile(r"^mslend you  = hit (\d+)", re.M)
MSL_THEM = re.compile(r"^mslend them = hit (\d+)", re.M)
# The game's fight census. See the definitions in src/vg/vg_ai.cpp.
FIGHT = re.compile(r"^fight \w+\s+frames (\d+) \| circle ([\d.]+)% \| extend (\d+) return (\d+)"
                   r" pass (\d+) shots (\d+) \| tail ([\d.]+)% shots (\d+)", re.M)
BREAK = re.compile(r"^break \w+\s+turnin (\d+) shots (\d+) hits (\d+)", re.M)
EXPOSED = re.compile(r"^break \w+.*\| exposed shots (\d+) hits (\d+)", re.M)
ILLUM = re.compile(r"^break \w+.*\| illum hold ([\d.]+)% letgo (\d+) hits (\d+)", re.M)
AGGR = re.compile(r"^aggr \w+\s+shots (\d+) solution (\d+) \| ended (\d+) wall (\d+) hits (\d+)"
                  r" \| evade ([\d.]+)%(?: \| solution hits (\d+))?", re.M)

# The weights, and the sign each part enters with. See the docstring for each.
# Set against the M2 network's own numbers (2026-10-25): it drew almost every
# bout, so the dense parts (damage, lock, reengage, dead) carry the gradient until
# kills appear, and a kill is worth the most once they do.
DEFAULT_WEIGHTS = {"kills": 1.0, "ttk": 0.5, "deaths": 1.0, "draws": 0.75,
                   "rams": 1.0, "seatwall": 0.0,
                   "damage": 2.0, "hit": 0.3, "armed": 0.5, "reengage": 0.02,
                   "dead": 0.5, "lock": 1.0, "guided": 0.5,
                   "circle": 0.0, "tail": 0.0, "tailshots": 0.0, "passes": 0.0,
                   "breakhits": 0.0, "breakshots": 0.0, "hitseat": 0.0,
                   "expohits": 0.0, "exposhots": 0.0,
                   "evade": 0.0, "solshots": 0.0, "wallshots": 0.0, "solhits": 0.0,
                   "illumhits": 0.0, "illumhold": 0.0, "letgo": 0.0}
SIGN = {"kills": 1, "ttk": 1, "deaths": -1, "draws": -1, "rams": -1, "seatwall": 0,
        "damage": 1, "hit": -1,
        "armed": 1, "reengage": 1, "dead": -1, "lock": 1, "guided": 1,
        "circle": -1, "tail": 1, "tailshots": 1, "passes": 1,
        "breakhits": 1, "breakshots": 1, "hitseat": 1, "expohits": 1, "exposhots": 1,
        "evade": 1, "solshots": 1, "wallshots": -1, "solhits": 1,
        "illumhits": 1, "illumhold": 1, "letgo": 1}


def span(text):
    out = []
    for part in str(text).split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    return out


# ---------------------------------------------------------------------------
# The network as one vector of numbers
# ---------------------------------------------------------------------------

def param_names(d):
    n_l = d.get("PILOT_NET_L", 2)
    names = []
    for l in range(n_l + 1):
        names += ["PILOT_W%d" % l, "PILOT_B%d" % l]
    return names


def flatten(a, names):
    return np.concatenate([a[n].astype(np.float64).ravel() for n in names])


def unflatten(theta, a, names):
    """Give a copy of the arrays with the weights taken from theta."""
    out = dict(a)
    i = 0
    for n in names:
        k = a[n].size
        out[n] = theta[i:i + k].reshape(a[n].shape)
        i += k
    return out


def scales(a, names):
    """The size of the noise for each number: the spread of its own layer."""
    return np.concatenate([np.full(a[n].size, max(float(np.std(a[n])), 1e-3))
                           for n in names])


def write_float_header(path, text, a, names):
    """Write the start header with the weight arrays replaced. The int8 part of the
    start header, if it has one, is left out: it belongs to the old weights.

    The numbers have 17 significant digits, so that the header gives back the
    float64 weights exactly. With fewer digits, quantize_pilot.py can make a
    different int8 network from the header than the run flew."""
    cut = text.find("// ---- THE INT8 NETWORK")
    if cut >= 0:
        text = text[:cut]
    for n in names:
        flat = np.asarray(a[n], dtype=np.float64).ravel()
        body = "\n".join("    %s," % ", ".join("%.16ef" % v for v in flat[i:i + 8])
                         for i in range(0, flat.size, 8))
        text, k = re.subn(r"static const float %s\[\]\s*=\s*\{.*?\};" % n,
                          "static const float %s[] = {\n%s\n};" % (n, body), text,
                          count=1, flags=re.S)
        if k != 1:
            raise ValueError("%s is not in the start header" % n)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def write_blob(path, d, a):
    q = qp.quantize(d, a)
    qp.write_blob(path, q, int(d.get("PILOT_MODE_N", 0)))


# ---------------------------------------------------------------------------
# Flying and scoring
# ---------------------------------------------------------------------------

def fly(job):
    # "tactics" is not a file: it flies the same fight with the network off, as the
    # control for an --eval.
    if job["blob"] == "tactics":
        net = ["--no-enemy-net"]
    else:
        net = ["--enemy-net", "--net-file", job["blob"]]
    cmd = [job["exe"], "--headless", "--gym", str(job["seat"]), str(job["enemy"]),
           "--bot"] + net + [
           "--pilot", str(job["level"]), "--seed", str(job["seed"]),
           "--frames", str(job["frames"]), "--bouts", str(job["bout"])]
    cmd += job["seat_flags"]
    cmd += job["flags"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if job.get("log"):
        # The whole output, so the cause of every bout can be read again later.
        os.makedirs(os.path.dirname(job["log"]), exist_ok=True)
        with open(job["log"], "w", encoding="utf-8") as f:
            f.write(" ".join(cmd) + "\n" + r.stdout)
    return r.stdout


def score_fight(text, enemy, frames, w, bout_limit, ram_rule=False):
    """The score of one fight and its parts. None if the run was short."""
    m = FRAMES.search(text)
    if not m or int(m.group(1)) != frames:
        return None
    minutes = frames / 3600.0
    bouts = BOUT.findall(text)
    # THE CAUSE DECIDES WHAT A BOUT IS WORTH. by= is what killed the loser.
    kills = [b for b in bouts if b[0] == "lost" and b[2] == "missile"]
    seatwall = sum(1 for b in bouts if b[0] == "lost" and b[2] in ("wall", "other"))
    rams = sum(1 for b in bouts if b[2] == "ram")
    deaths = sum(1 for b in bouts if b[0] == "won")
    draws = sum(1 for b in bouts if b[0] == "draw")
    # With the ram rule, a bout that ends in a ram gives no credit for damage done.
    dealt = sum(float(b[4]) - float(b[5]) for b in bouts
                if not (ram_rule and b[2] == "ram"))
    taken = sum(1.0 - float(b[8]) for b in bouts)
    ttk = sum(max(0.0, 1.0 - float(b[1]) / bout_limit) for b in kills)
    hit = 0
    mh = MSL_YOU.search(text)
    if mh:
        hit = int(mh.group(1))
    armed = reengage = dead = lock = guided = 0.0
    c = CENSUS.search(text)
    if c:
        lock = int(c.group(3)) / 100.0
        armed = float(c.group(4)) / 100.0
        reengage = int(c.group(5))
        dead = int(c.group(6)) / 100.0
    if enemy == 3:
        ms = MSL.search(text)
        if ms:
            ended = sum(int(x) for x in ms.groups())
            guided = int(ms.group(1)) / ended if ended else 0.0
    circle = tail = 0.0
    passes = tailshots = breakhits = breakshots = hitseat = 0
    f = FIGHT.search(text)
    if f:
        circle = float(f.group(2)) / 100.0
        passes = int(f.group(5))
        tail = float(f.group(7)) / 100.0
        tailshots = int(f.group(8))
    bk = BREAK.search(text)
    if bk:
        breakshots = int(bk.group(2))
        breakhits = int(bk.group(3))
    exposhots = expohits = 0
    ex = EXPOSED.search(text)
    if ex:
        exposhots = int(ex.group(1))
        expohits = int(ex.group(2))
    mt = MSL_THEM.search(text)
    if mt:
        hitseat = int(mt.group(1))
    evade = 0.0
    solshots = wallshots = solhits = 0
    ag = AGGR.search(text)
    if ag:
        solshots = int(ag.group(2))
        wallshots = int(ag.group(4))
        hitseat = int(ag.group(5))     # the same count, by shooter class
        evade = float(ag.group(6)) / 100.0
        solhits = int(ag.group(7) or 0)
    illumhold = 0.0
    letgo = illumhits = 0
    il = ILLUM.search(text)
    if il:
        illumhold = float(il.group(1)) / 100.0
        letgo = int(il.group(2))
        illumhits = int(il.group(3))
    parts = {"kills": len(kills) / minutes, "ttk": ttk / minutes,
             "deaths": deaths / minutes, "draws": draws / minutes,
             "rams": rams / minutes, "seatwall": seatwall / minutes,
             "damage": (dealt - taken) / minutes, "hit": hit / minutes,
             "armed": armed, "reengage": reengage / minutes, "dead": dead,
             "lock": lock, "guided": guided,
             "circle": circle, "tail": tail, "tailshots": tailshots / minutes,
             "passes": passes / minutes, "breakhits": breakhits / minutes,
             "breakshots": breakshots / minutes, "hitseat": hitseat / minutes,
             "exposhots": exposhots / minutes, "expohits": expohits / minutes,
             "evade": evade, "solshots": solshots / minutes, "wallshots": wallshots / minutes,
             "solhits": solhits / minutes, "illumhits": illumhits / minutes,
             "illumhold": illumhold, "letgo": letgo / minutes}
    total = sum(SIGN[k] * w[k] * parts[k] for k in SIGN)
    return total, parts


def evaluate(blobs, fights, args, w, pool, log_dir=None, log_blobs=None):
    """Fly every blob in every fight. Give a score for each blob, and its parts.

    With log_dir, the output of each fight is kept there, one file a fight: for
    every blob, or only for the blobs in log_blobs when that is given.
    """
    jobs = []
    for bi, blob in enumerate(blobs):
        for fi, f in enumerate(fights):
            job = dict(f, blob=blob, bi=bi)
            if log_dir and (log_blobs is None or blob in log_blobs):
                job["log"] = os.path.join(
                    log_dir, os.path.splitext(os.path.basename(blob))[0],
                    "s%d_e%d_L%d_seed%d_n%d.txt" % (f["seat"], f["enemy"], f["level"],
                                                    f["seed"], fi))
            jobs.append(job)
    texts = list(pool.map(fly, jobs))
    per = [[[] for _ in CLASSES] for _ in blobs]
    cls = [[dict.fromkeys(SIGN, 0.0) for _ in CLASSES] for _ in blobs]
    n_cls = [[0.0] * len(CLASSES) for _ in blobs]
    short = 0
    ram_rule = getattr(args, "ram_rule", False)
    for job, text in zip(jobs, texts):
        s = score_fight(text, job["enemy"], job["frames"], w, job["bout"], ram_rule)
        if s is None:
            short += 1
            continue
        # Each fight counts with the weight of its seat (see --seat-kinds).
        bi, e, sw = job["bi"], job["enemy"], job.get("seat_w", 1.0)
        per[bi][e].append((s[0], sw))
        for k, v in s[1].items():
            cls[bi][e][k] += v * sw
        n_cls[bi][e] += sw
    # Each class apart, then added, so a weak class counts as much as a strong one.
    scores = np.array([sum(sum(x * sw for x, sw in c) / sum(sw for _, sw in c) if c else 0.0
                           for c in p) for p in per])
    cls = [[{k: v / max(n, 1e-9) for k, v in c.items()} for c, n in zip(cb, nb)]
           for cb, nb in zip(cls, n_cls)]
    parts = [{k: sum(c[k] for c, n in zip(cb, nb) if n) / max(sum(1 for n in nb if n), 1)
              for k in SIGN} for cb, nb in zip(cls, n_cls)]
    return scores, parts, short, cls


def seat_list(args):
    """The seats as (name, game flags, weight). See THE LEAGUE in the docstring."""
    if not getattr(args, "seat_kinds", None):
        return [("scripted", ["--scripted"], 1.0)] + [
            ("net%d" % i, ["--seat-net-file", os.path.abspath(p)], 1.0)
            for i, p in enumerate(args.seat_net)]
    blob = os.path.abspath(args.seat_blob)
    kinds = {"scripted": ["--scripted"], "plain": ["--seat-net-file", blob],
             "curve": ["--seat-net-file", blob, "--seat-author-curve"]}
    out = []
    for part in args.seat_kinds.split(","):
        name, _, weight = part.partition(":")
        if name not in kinds:
            raise SystemExit("error: no seat kind called %s" % name)
        out.append((name, kinds[name], float(weight or 1.0)))
    return out


def fight_list(args, exe, frames, flags, seats, seed0):
    return [{"exe": exe, "seat": s, "enemy": e, "level": lv, "seed": seed0 + k + 1,
             "frames": frames, "bout": args.bout, "flags": flags,
             "seat_kind": name, "seat_flags": sf, "seat_w": sw}
            for s in span(args.seats) for e in span(args.enemies)
            for lv in span(args.levels) for k in range(args.seeds)
            for name, sf, sw in seats]


def cmd_eval(args):
    """Score network files on fixed fights, and print the parts of the score."""
    w = dict(DEFAULT_WEIGHTS)
    for part in filter(None, args.weights.split(",")):
        k, v = part.split("=")
        w[k] = float(v)
    exe = os.path.abspath(args.exe)
    frames = int(round(args.minutes * 3600))
    fights = fight_list(args, exe, frames, args.flags.split(), seat_list(args), args.seed0)
    blobs = [b if b == "tactics" else os.path.abspath(b) for b in args.eval]
    log_dir = os.path.join(OUT_ROOT, args.name, "fights") if args.name else None
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        scores, parts, short, cls = evaluate(blobs, fights, args, w, pool, log_dir)
    print("%d fights for each network, %g min each, %s. Short runs: %d."
          % (len(fights), args.minutes, args.flags, short))
    print("Per 10 min: missile kills, ttk bonus, deaths, draws, rams (any outcome),")
    print("seat lost to the wall, hits on the network by seat missiles, hull exchange.")
    print("Shares: lock, armed, dead (4 s past the last lock). reengage: new passes a minute.")
    print("Census: circle and tail shares; passes, tail shots, break hits per 10 min;")
    print("exch = hits on the seat / hits on the network (the check for pressure).")
    print("Aggression: hitS = hits on the seat per 10 min, evade = share of frames the")
    print("seat must evade (one of its locked rounds 2 s or less away).")
    head = ("%-22s %-9s %6s %5s %6s %6s %5s %5s %6s %6s %6s %6s %6s %6s"
            " %6s %6s %6s %6s %6s %6s %6s %6s")
    print(head % ("network", "class", "kills", "ttk", "deaths", "draws", "rams",
                  "swall", "hit", "dmg", "lock", "armed", "reeng", "dead",
                  "circ", "pass", "tail", "tshot", "bhit", "exch", "hitS", "evade") + "  score")
    result = []
    for b, sc, pa, cb in zip(blobs, scores, parts, cls):
        name = os.path.basename(b)
        rows = [("all", pa)] + [(CLASSES[i], cb[i]) for i in span(args.enemies)]
        for label, p in rows:
            print(head % (name[:22], label, "%.2f" % (10 * p["kills"]),
                          "%.2f" % (10 * p["ttk"]), "%.2f" % (10 * p["deaths"]),
                          "%.2f" % (10 * p["draws"]), "%.2f" % (10 * p["rams"]),
                          "%.2f" % (10 * p["seatwall"]), "%.1f" % (10 * p["hit"]),
                          "%+.2f" % (10 * p["damage"]), "%.0f%%" % (100 * p["lock"]),
                          "%.0f%%" % (100 * p["armed"]),
                          "%.2f" % p["reengage"], "%.0f%%" % (100 * p["dead"]),
                          "%.0f%%" % (100 * p["circle"]), "%.2f" % (10 * p["passes"]),
                          "%.1f%%" % (100 * p["tail"]), "%.2f" % (10 * p["tailshots"]),
                          "%.2f" % (10 * p["breakhits"]),
                          "%.2f" % (p["hitseat"] / p["hit"]) if p["hit"] > 0 else "-",
                          "%.1f" % (10 * p["hitseat"]), "%.1f%%" % (100 * p["evade"]))
                  + ("  %.3f" % sc if label == "all" else ""))
        result.append({"blob": b, "score": float(sc), "parts": pa, "classes": cb})
    if args.name:
        out = os.path.join(OUT_ROOT, args.name)
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "eval.json"), "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
    return 0


def centered_ranks(x):
    """Ranks scaled to -0.5 .. 0.5. Using ranks, not raw scores, keeps one lucky
    fight from pulling the whole generation."""
    r = np.empty(len(x))
    r[np.argsort(x)] = np.arange(len(x))
    return r / max(len(x) - 1, 1) - 0.5


def main():
    ap = argparse.ArgumentParser(
        description="Improve a pilot network by evolution in the headless game.")
    ap.add_argument("--start", help="the float header to start from")
    ap.add_argument("--name", help="folder name under captures/ai_eval/es")
    ap.add_argument("--resume", type=int, metavar="GEN",
                    help="continue the run in --name at generation GEN. The weights come "
                         "from its gen_GEN.h, and generation GEN is flown again, because "
                         "the run does not keep the scores of its copies. Use the same "
                         "--start, --seed and --pop as the first part of the run")
    ap.add_argument("--exe", required=True,
                    help="the game, built from a tree that has --net-file")
    ap.add_argument("--gens", type=int, default=50, help="generations (default 50)")
    ap.add_argument("--pop", type=int, default=24,
                    help="copies for each generation, an even number (default 24)")
    ap.add_argument("--sigma", type=float, default=0.03,
                    help="noise, as a share of each layer's spread (default 0.03)")
    ap.add_argument("--lr", type=float, default=0.02,
                    help="step size, as a share of each layer's spread (default 0.02)")
    ap.add_argument("--anchor", type=float, default=0.01,
                    help="the pull back toward the start each generation (default 0.01)")
    ap.add_argument("--seats", default="0-3", help="seat classes (default 0-3)")
    ap.add_argument("--enemies", default="0-3",
                    help="classes the network flies (default 0-3)")
    ap.add_argument("--levels", default="2", help="pilot levels (default 2, PRO)")
    ap.add_argument("--seeds", type=int, default=2,
                    help="seeds for each fight, new ones each generation (default 2)")
    # A bout that is still in the air when the fight stops is not scored, so a
    # fight must hold several bouts: 6 minutes against a 120 s limit is at least 3.
    ap.add_argument("--minutes", type=float, default=6.0,
                    help="simulated minutes for each fight (default 6)")
    ap.add_argument("--bout", type=float, default=120.0,
                    help="seconds after which a bout is a draw (default 120)")
    ap.add_argument("--seat-net", action="append", default=[],
                    help="a network file to fly the seat, in addition to the scripted "
                         "seat. Give it more than once for more. Not with --seat-kinds")
    ap.add_argument("--seat-kinds",
                    help="the seats and their weights, for example "
                         "scripted:0.5,plain:1,curve:1 (see THE LEAGUE)")
    ap.add_argument("--seat-blob", default="captures/ai_eval/m3/mixed32F.bin",
                    help="with --seat-kinds: the network file of the plain and curve seats "
                         "(default: the M2 imitation of the author)")
    ap.add_argument("--ram-rule", action="store_true",
                    help="no credit for damage done in a bout that ends in a ram")
    ap.add_argument("--flags",
                    default="--net-all-phases --net-rules-trigger --no-modes --net-corner-exempt",
                    help="game flags for every fight (default: the network flies every "
                         "phase, the rules fire, no mode head, and the cornered pull "
                         "leaves the network alone)")
    ap.add_argument("--weights", default="",
                    help="score weights, for example kills=1,draws=0.5")
    ap.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="fights at the same time (default: half the processors)")
    ap.add_argument("--seed", type=int, default=0, help="seed of the noise")
    ap.add_argument("--seed0", type=int, default=0,
                    help="with --eval: the fights use seeds seed0+1 .. seed0+--seeds. "
                         "Training uses 1001 and up, so keep this under 1000 (default 0)")
    ap.add_argument("--eval", nargs="+", metavar="BLOB",
                    help="do not train: score these network files on fixed fights "
                         "(seeds 1 .. --seeds) and print the parts of the score. "
                         "The name tactics flies the same fights with the network off")
    args = ap.parse_args()
    if args.eval:
        return cmd_eval(args)
    if not args.start or not args.name:
        print("error: --start and --name are necessary to train")
        return 1

    if args.pop % 2:
        print("error: --pop must be even (the samples are mirrored)")
        return 1
    w = dict(DEFAULT_WEIGHTS)
    for part in filter(None, args.weights.split(",")):
        k, v = part.split("=")
        if k not in w:
            print("error: no score part called %s" % k)
            return 1
        w[k] = float(v)

    text, d, a = qp.read_header(args.start)
    names = param_names(d)
    theta0 = flatten(a, names)
    theta = theta0.copy()
    sc = scales(a, names)
    out = os.path.join(OUT_ROOT, args.name)
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    first = 0
    if args.resume is not None:
        # gen_NNN.npy and gen_NNN.h hold the weights at the START of generation NNN.
        # The start header still sets theta0 and the noise scale.
        first = args.resume
        resume_npy = os.path.join(out, "gen_%03d.npy" % first)
        resume_h = os.path.join(out, "gen_%03d.h" % first)
        if os.path.isfile(resume_npy):
            source = resume_npy
            theta = np.load(resume_npy)
            if theta.shape != theta0.shape:
                print("error: %s does not fit the start header" % resume_npy)
                return 1
        elif os.path.isfile(resume_h):
            source = resume_h
            # ONLY 7 DECIMALS in the header. The weights are about 1e-7 from the
            # weights of the first part, which is enough to change some int8
            # multipliers of the copies. Generation NNN then flies differently.
            _, _, ar = qp.read_header(resume_h)
            theta = flatten(ar, names)
            print("note: no %s, so the weights come from the header (7 decimals)"
                  % os.path.basename(resume_npy))
        else:
            print("error: %s does not exist" % resume_h)
            return 1
        # The same noise as the first part of the run: one draw for each generation
        # before this one.
        for _ in range(first):
            rng.standard_normal((args.pop // 2, theta.size))
        print("continue at generation %d from %s" % (first, source))
    exe = os.path.abspath(args.exe)
    frames = int(round(args.minutes * 3600))
    flags = args.flags.split()
    seats = seat_list(args)
    print("%d weights, %d copies a generation, %s" % (theta.size, args.pop, " ".join(flags)))
    print("seats: %s%s" % (", ".join("%s x%g" % (n, sw) for n, _, sw in seats),
                           ", ram rule" if args.ram_rule else ""))

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for gen in range(first, args.gens):
            t0 = time.time()
            # The same fights for every copy in a generation, new fights each generation.
            fights = fight_list(args, exe, frames, flags, seats, 1000 * (gen + 1))
            half = args.pop // 2
            eps = rng.standard_normal((half, theta.size))
            eps = np.concatenate([eps, -eps])
            blobs = []
            for i in range(args.pop):
                p = os.path.join(out, "cand_%02d.bin" % i)
                write_blob(p, d, unflatten(theta + args.sigma * sc * eps[i], a, names))
                blobs.append(p)
            mean_blob = os.path.join(out, "gen_%03d.bin" % gen)
            write_blob(mean_blob, d, unflatten(theta, a, names))
            # The fights of the generation's own network are kept (one file a fight, not
            # the whole population's), so the causes of its bouts can be read later.
            scores, parts, short, _ = evaluate(blobs + [mean_blob], fights, args, w, pool,
                                               os.path.join(out, "fights"), {mean_blob})
            cand, mean_score = scores[:-1], scores[-1]
            write_float_header(os.path.join(out, "gen_%03d.h" % gen), text,
                               unflatten(theta, a, names), names)
            # The exact weights, for --resume.
            np.save(os.path.join(out, "gen_%03d.npy" % gen), theta)
            # The step: toward the copies that ranked well, then back toward the start.
            g = (centered_ranks(cand)[:, None] * eps).sum(axis=0) / args.pop
            theta = theta + args.lr * sc * g - args.anchor * (theta - theta0)
            line = {"gen": gen, "mean_score": float(mean_score),
                    "best": float(cand.max()), "median": float(np.median(cand)),
                    "parts": parts[-1], "short": short, "fights": len(fights),
                    "seconds": round(time.time() - t0, 1),
                    "drift": float(np.sqrt(np.mean(((theta - theta0) / sc) ** 2)))}
            if args.resume is not None:
                line["resumed_at"] = args.resume
            with open(os.path.join(out, "log.jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps(line) + "\n")
            pm = parts[-1]
            print("gen %3d  score %.3f  best copy %.3f  /10 min: kills %.2f deaths %.2f "
                  "draws %.2f rams %.2f swall %.2f bhits %.2f  circle %.0f%%  exch %s  "
                  "drift %.3f  short %d  %.0f s"
                  % (gen, mean_score, cand.max(), 10 * pm["kills"], 10 * pm["deaths"],
                     10 * pm["draws"], 10 * pm["rams"], 10 * pm["seatwall"],
                     10 * pm["breakhits"], 100 * pm["circle"],
                     "%.2f" % (pm["hitseat"] / pm["hit"]) if pm["hit"] > 0 else "-",
                     line["drift"], short, line["seconds"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
