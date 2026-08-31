"""Look at what the pilots are DOING.

tools/inspect_pilot.py looks inside the trained network. This looks at behaviour,
which is a different question and is the one that matters while the network is
off: which decision each class spends its frames in, whether it can aim, whether
it ever has a shot to take.

It runs the headless build over a matrix of matchups, reads the census the game
prints on the way out, and writes one HTML page.

    python tools/inspect_ai.py
    python tools/inspect_ai.py --seeds 11,23,37 --out ai.html
    python tools/inspect_ai.py --extra --no-enemy-net

The last form is how to compare two AI configurations: run it twice with
different flags and put the pages side by side.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "host", "build", "phantom.exe")
DEF_OUT = os.path.join(ROOT, "ai_report.html")

CLASSES = ["AEGIS", "LANCE", "CHARIOT", "BALLISTA"]
# What a pilot can be doing. The order matches the enum in vg_game.h.
KINDS = ["-", "WALL", "RAM", "EVADE", "TAIL", "DRY", "PRESS", "NET", "TACTIC",
         "RESET", "CORNER", "SUPPORT"]
# What each one means, for the page.
MEANS = {
    "WALL": "turning away from the boundary",
    "RAM": "committed to a suicide run",
    "EVADE": "breaking across an incoming round",
    "TAIL": "answering somebody behind it",
    "DRY": "rack empty, leaving",
    "PRESS": "an aimed shot: nose on the target",
    "NET": "flown by the trained network",
    "TACTIC": "its own class plan",
    "RESET": "breaking a stalemate on purpose",
    "CORNER": "hurt, and coming anyway",
    "SUPPORT": "holding a shot already in the air",
}


def run(player, enemy, seed, extra):
    """One headless match. Returns the census lines it printed."""
    cmd = [EXE, "--headless", "--bot", "--scripted", "--gym", str(player),
           str(enemy), *extra, "--seed", str(seed), "--frames", "30000"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        m = re.match(r"ai (\w+)\s+frames (\d+) \| aim ([-\d.]+) \| locked (\d+)%"
                     r" \| armed ([\d.]+)% \| mean range (\d+) \|(.*)", line)
        if not m:
            continue
        name = m.group(1)
        kinds = dict((k, float(v)) for k, v in re.findall(r"(\w+)=(\d+)%", m.group(7)))
        out[name] = dict(frames=int(m.group(2)), aim=float(m.group(3)),
                         locked=float(m.group(4)), armed=float(m.group(5)),
                         rng=float(m.group(6)), kinds=kinds)
    return out


def gather(seeds, extra):
    """Every class, as the enemy, against every player ship."""
    acc = {}
    for e, name in enumerate(CLASSES):
        rows = []
        for p in range(len(CLASSES)):
            for sd in seeds:
                got = run(p, e, sd, extra)
                if name in got:
                    rows.append(got[name])
        if not rows:
            continue
        n = float(len(rows))
        kinds = {}
        for r in rows:
            for k, v in r["kinds"].items():
                kinds[k] = kinds.get(k, 0.0) + v / n
        acc[name] = dict(
            frames=sum(r["frames"] for r in rows),
            aim=sum(r["aim"] for r in rows) / n,
            locked=sum(r["locked"] for r in rows) / n,
            armed=sum(r["armed"] for r in rows) / n,
            rng=sum(r["rng"] for r in rows) / n,
            kinds=kinds, runs=int(n))
    return acc


CSS = """
:root{--ground:#edeae0;--panel:#f7f5ef;--rule:#d6d1c2;--soft:#e3dfd2;--ink:#1c1a16;
--dim:#6f685a;--amber:#b8730a;--bad:#a63a24;--good:#3f6b32}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
--ground:#0d0d0f;--panel:#16161a;--rule:#2b2b33;--soft:#212129;--ink:#e8e1d2;
--dim:#958f80;--amber:#ffb02e;--bad:#ff7a5c;--good:#8fd07a}}
:root[data-theme="dark"]{--ground:#0d0d0f;--panel:#16161a;--rule:#2b2b33;
--soft:#212129;--ink:#e8e1d2;--dim:#958f80;--amber:#ffb02e;--bad:#ff7a5c;--good:#8fd07a}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);padding:44px 24px 80px;
font:15px/1.6 ui-monospace,"SFMono-Regular",Menlo,Consolas,monospace}
.wrap{max-width:960px;margin:0 auto;display:flex;flex-direction:column;gap:30px}
h1{font-size:20px;margin:0;letter-spacing:.14em;text-transform:uppercase;color:var(--amber)}
.sub{color:var(--dim);margin:6px 0 0;font-size:13px}
h2{font-size:12px;margin:0 0 12px;letter-spacing:.16em;text-transform:uppercase;color:var(--dim)}
section{background:var(--panel);border:1px solid var(--rule);padding:18px 20px}
.bar{display:flex;height:26px;border:1px solid var(--rule);overflow:hidden;margin:4px 0 8px}
.seg{display:flex;align-items:center;justify-content:center;font-size:10px;
overflow:hidden;white-space:nowrap;color:#0d0d0f;font-weight:600}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:4px 10px 4px 0;text-align:right;font-variant-numeric:tabular-nums}
td:first-child,th:first-child{text-align:left}
th{color:var(--dim);font-weight:400;font-size:10.5px;letter-spacing:.1em;
text-transform:uppercase;border-bottom:1px solid var(--rule)}
.name{color:var(--amber);font-weight:600}
.note{color:var(--dim);font-size:12.5px;max-width:70ch;margin:12px 0 0;line-height:1.6}
.flag{color:var(--bad)}
.key{display:flex;flex-wrap:wrap;gap:10px 18px;margin-top:10px;font-size:11.5px;color:var(--dim)}
.key i{display:inline-block;width:9px;height:9px;margin-right:5px;font-style:normal}
"""

# One colour per decision, warm for fighting and cool for not.
COLOUR = {"WALL":"#7d6b52","RAM":"#a63a24","EVADE":"#c58a3d","TAIL":"#8a7f5c",
          "DRY":"#5f6b7a","PRESS":"#ffb02e","NET":"#6f9bd1","TACTIC":"#c9932f",
          "RESET":"#8d8676","CORNER":"#b4553a","SUPPORT":"#d9a441","-":"#3a3a42"}


def render(acc, seeds, extra):
    h = ["<title>Pilot Behaviour</title><style>%s</style>" % CSS, '<div class="wrap">']
    h.append("<header><h1>Pilot behaviour</h1><p class='sub'>%d seeds, every class "
             "against every player ship%s</p></header>"
             % (len(seeds), (" &middot; " + " ".join(extra)) if extra else ""))

    for name in CLASSES:
        d = acc.get(name)
        if not d:
            continue
        h.append("<section>")
        h.append('<h2>%s</h2>' % name)
        h.append('<div class="bar">')
        for k in KINDS:
            v = d["kinds"].get(k, 0.0)
            if v < 0.7:
                continue
            h.append('<div class="seg" style="width:%.2f%%;background:%s">%s</div>'
                     % (v, COLOUR.get(k, "#666"), k if v > 7 else ""))
        h.append("</div>")
        h.append("<table><tr><th>decision</th><th>share</th><th>meaning</th></tr>")
        for k, v in sorted(d["kinds"].items(), key=lambda x: -x[1]):
            if v < 0.7:
                continue
            h.append('<tr><td class="name">%s</td><td>%.0f%%</td>'
                     '<td style="text-align:left;color:var(--dim)">%s</td></tr>'
                     % (k, v, MEANS.get(k, "")))
        h.append("</table>")
        aim_flag = ' class="flag"' if d["aim"] < 0.5 else ""
        h.append('<table style="margin-top:12px">'
                 '<tr><th>aim (cos to target)</th><th>locked</th><th>armed</th>'
                 '<th>mean range</th><th>runs</th></tr>'
                 '<tr><td%s>%.3f</td><td>%.0f%%</td><td>%.1f%%</td>'
                 '<td>%.0f</td><td>%d</td></tr></table>'
                 % (aim_flag, d["aim"], d["locked"], d["armed"], d["rng"], d["runs"]))
        h.append('<p class="note">A pilot can only lock what its nose is pointed at: '
                 'aim_dir is s-&gt;fwd plus the pilot error, so this cosine IS the '
                 'firing solution. 1.0 is straight at the target; the lock cone is '
                 'about 0.86. Anything under 0.5 means the class is flying, not '
                 'aiming.</p>')
        h.append("</section>")

    h.append('<section><h2>Reading it</h2>'
             '<p class="note">A class whose own TACTIC share is small is not flying its '
             'own plan -- something above it in the chain is taking the frames. That is '
             'how the network was caught: it held 67% of an AEGIS\'s frames and left 0% '
             'for evade, tail, extend, press and the class tactic together.</p>'
             '<p class="note">WALL is the arena tax. It was 27-34% of every pilot\'s '
             'frames when the tube was 1100 units across, because they turn inward 700 '
             'units from a wall that was only 1100 away.</p>'
             '<p class="note">DRY is time spent with an empty rack. High DRY with low '
             'ARMED means a class is spending its magazine faster than it can earn '
             'firing solutions.</p></section>')
    h.append("</div>")
    return "\n".join(h)


def main():
    ap = argparse.ArgumentParser(description="Look at what the pilots are doing.")
    ap.add_argument("--seeds", default="11,23", help="comma separated (default 11,23)")
    ap.add_argument("--out", default=DEF_OUT)
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="flags to pass to the game, e.g. --extra --no-enemy-net")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        print("error: build the PC target first")
        return 1
    seeds = [int(x) for x in args.seeds.split(",") if x.strip()]
    extra = [a for a in args.extra if a]
    total = len(CLASSES) * len(CLASSES) * len(seeds)
    print("running %d matches..." % total)
    acc = gather(seeds, extra)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(render(acc, seeds, extra))
    print("wrote %s" % args.out)
    for name in CLASSES:
        d = acc.get(name)
        if d and d["aim"] < 0.5:
            print("  note: %s holds its aim %.3f off the target - it is flying, "
                  "not aiming" % (name, d["aim"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
