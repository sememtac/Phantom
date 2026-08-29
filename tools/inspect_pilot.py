"""Look inside a trained pilot network.

The network is a black box, and two faults have already hidden in it. One field
of the observation was read as another after the layout moved. A class stopped
being recognised after a balance change, so the rules flew it and nothing said
so. Both were visible in the weights. Neither was visible in the game.

This script reads a generated header and writes one HTML page. Open the page in
a browser. It does not need a network connection.

    python tools/inspect_pilot.py
    python tools/inspect_pilot.py --net other_net.h --out report.html
    python tools/inspect_pilot.py captures/a_aegis.obs

Give it dump files as well and it adds what the modes look like in that flying.
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_NET = os.path.join(ROOT, "src", "vg", "generated", "pilot_net.h")
DEF_OUT = os.path.join(ROOT, "pilot_report.html")
EXE = os.path.join(ROOT, "host", "build", "phantom.exe")
BOT_H = os.path.join(ROOT, "src", "vg", "vg_bot.h")
MODES_H = os.path.join(ROOT, "src", "vg", "vg_modes.h")


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def defines(text):
    """Every #define with a number, as a dictionary."""
    out = {}
    for name, val in re.findall(r"#define\s+(\w+)\s+([-0-9.]+)f?\s*$", text, re.M):
        out[name] = float(val) if "." in val else int(val)
    return out


def arrays(text):
    """Every float array in the header, as a dictionary of numpy arrays."""
    out = {}
    for name, body in re.findall(r"static const float (\w+)\[\]\s*=\s*\{(.*?)\};",
                                 text, re.S):
        nums = re.findall(r"-?\d+\.\d+(?:[eE][-+]?\d+)?", body)
        out[name] = np.array([float(x) for x in nums], dtype=np.float64)
    return out


def obs_names(path=BOT_H):
    """The observation field names, in order, from the enum in vg_bot.h."""
    text = read_text(path)
    m = re.search(r"enum\s*\{(.*?)\};", text, re.S)
    if not m:
        return []
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = []
    for part in body.split(","):
        part = part.strip().split("=")[0].strip()
        if part.startswith("OBS_"):
            names.append(part[4:])
    return names


def mode_names(path=MODES_H):
    return [m[1] for m in re.findall(r'X\((\w+),\s*"([^"]+)"\)', read_text(path))]


def live_airframes():
    """Ask the game what the ship gate sees. Returns a list of name and values."""
    if not os.path.exists(EXE):
        return []
    try:
        r = subprocess.run([EXE, "--airframes"], capture_output=True, text=True,
                           timeout=30)
    except Exception:
        return []
    rows = []
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) > 2 and parts[0] == "airframe":
            rows.append((parts[1], np.array([float(x) for x in parts[2:]])))
    return rows


def gate_check(seen, tol, live, ship_n):
    """For each live class, how far it is from the nearest class the net knows.

    The gate compares field by field and takes the worst one, so that is what
    this reports. A class further than the tolerance is refused, and then the
    hand written tactics fly it and nothing in the game says so.
    """
    rows = []
    if seen.size == 0 or not live:
        return rows
    table = seen.reshape(-1, ship_n)
    for name, vals in live:
        d = np.abs(table - vals[None, :]).max(axis=1)
        k = int(d.argmin())
        worst = int(np.abs(table[k] - vals).argmax())
        rows.append({"name": name, "dist": float(d[k]), "row": k, "field": worst,
                     "ok": float(d[k]) <= tol})
    return rows


def input_use(w0, n_in, names):
    """How much weight the first layer puts on each input.

    The values are already scaled to a common size before they reach the layer,
    so the size of the weights is a fair measure of how much the network reads a
    field. A field near zero is a field the network ignores.
    """
    w = np.abs(w0.reshape(-1, n_in)).sum(axis=0)
    w = w / max(w.max(), 1e-9)
    return sorted(({"name": names[i] if i < len(names) else "input %d" % i,
                    "use": float(w[i]), "index": i} for i in range(n_in)),
                  key=lambda r: -r["use"])


def dead_units(w_out, n_h):
    """Hidden units whose output weights are all near zero. They do nothing."""
    w = np.abs(w_out.reshape(-1, n_h)).sum(axis=0)
    lim = 0.05 * max(w.max(), 1e-9)
    return int((w < lim).sum())


def mode_stats(paths, look, smooth):
    """What the modes look like in some recorded flying."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "tp", os.path.join(ROOT, "tools", "train_pilot.py"))
    tp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tp)
    tot = np.zeros(len(tp.MODE_NAMES))
    dwell = []
    for p in paths:
        obs, _, _, _ = tp.read_dataset(p)
        m = tp.label_modes(obs, look, smooth)
        if m.size == 0:
            continue
        for k in range(len(tp.MODE_NAMES)):
            tot[k] += int((m == k).sum())
        ch = np.flatnonzero(np.diff(m)) + 1
        dwell.extend(np.diff(np.concatenate([[0], ch, [len(m)]])).tolist())
    if not dwell:
        return None
    d = np.array(dwell)
    return {"names": tp.MODE_NAMES, "share": (tot / max(tot.sum(), 1)).tolist(),
            "median": float(np.median(d) / 60.0), "mean": float(d.mean() / 60.0),
            "p90": float(np.percentile(d, 90) / 60.0),
            "flicker": float((d < 30).mean())}


CSS = """
:root{--bg:#f6f3ec;--panel:#fffdf8;--line:#ddd5c4;--ink:#221d17;--dim:#6d6355;
--amber:#a86a00;--bad:#b4361f;--ok:#3f6b32}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){
--bg:#0b0b0c;--panel:#151517;--line:#2a2a2e;--ink:#e8dcc4;--dim:#8a8377;
--amber:#ffb000;--bad:#ff6a4d;--ok:#8fd07a}}
:root[data-theme=dark]{--bg:#0b0b0c;--panel:#151517;--line:#2a2a2e;--ink:#e8dcc4;
--dim:#8a8377;--amber:#ffb000;--bad:#ff6a4d;--ok:#8fd07a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);padding:28px 20px 64px;
font:14px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.wrap{max-width:960px;margin:0 auto;display:flex;flex-direction:column;gap:26px}
h1{font-size:19px;margin:0;letter-spacing:.14em;text-transform:uppercase;color:var(--amber)}
h2{font-size:12px;margin:0 0 10px;letter-spacing:.18em;text-transform:uppercase;color:var(--dim)}
.sub{color:var(--dim);margin:4px 0 0}
section{background:var(--panel);border:1px solid var(--line);padding:16px 18px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
td,th{text-align:left;padding:3px 12px 3px 0;white-space:nowrap}
th{color:var(--dim);font-weight:400;border-bottom:1px solid var(--line)}
.num{text-align:right}
.bad{color:var(--bad)}.ok{color:var(--ok)}.amber{color:var(--amber)}.dim{color:var(--dim)}
.bars{font-size:12px;line-height:1.5;overflow-x:auto}
.bars div{white-space:pre}
.note{color:var(--dim);margin:12px 0 0;white-space:normal;max-width:68ch}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:14px}
.stat b{display:block;font-size:20px;color:var(--amber);font-weight:500}
.stat span{color:var(--dim);font-size:12px}
"""

OBS = []


def bar(frac, width=34):
    """A bar drawn with block characters, for the input list."""
    n = int(round(max(0.0, min(1.0, frac)) * width))
    return "#" * n + "." * (width - n)


def render(d, a, gate, use, modes, rec, net_path):
    n_in = d.get("PILOT_NET_IN", 0)
    n_h = d.get("PILOT_NET_H", 0)
    n_out = d.get("PILOT_NET_OUT", 0)
    n_l = d.get("PILOT_NET_L", 2)
    n_mode = d.get("PILOT_MODE_N", 0)
    ship_n = d.get("PILOT_SHIP_N", 11)
    weights = sum(v.size for k, v in a.items()
                  if k.startswith("PILOT_W") or k.startswith("PILOT_B"))
    h = []
    h.append("<title>Pilot Net Inspector</title><style>%s</style>" % CSS)
    h.append('<div class="wrap">')
    h.append('<header><h1>Pilot net inspector</h1><p class="sub">%s</p></header>'
             % os.path.basename(net_path))

    h.append('<section><h2>Shape</h2><div class="grid">')
    for label, val in (("inputs", n_in), ("hidden", "%d x %d" % (n_h, n_l)),
                       ("outputs", n_out), ("modes", n_mode or "none"),
                       ("weights", "%d" % weights),
                       ("flash", "%.1f KB" % (weights * 4 / 1024.0))):
        h.append('<div class="stat"><b>%s</b><span>%s</span></div>' % (val, label))
    h.append("</div></section>")

    h.append("<section><h2>Ship gate</h2>")
    if gate:
        h.append("<table><tr><th>class</th><th>worst field</th>"
                 '<th class="num">distance</th><th>verdict</th></tr>')
        for r in gate:
            k = r["field"] + n_in - ship_n
            fld = OBS[k] if 0 <= k < len(OBS) else "field %d" % r["field"]
            cls = "ok" if r["ok"] else "bad"
            note = "flown by the network" if r["ok"] else "REFUSED - the rules fly it"
            h.append('<tr><td class="amber">%s</td><td class="dim">%s</td>'
                     '<td class="num">%.4f</td><td class="%s">%s</td></tr>'
                     % (r["name"], fld, r["dist"], cls, note))
        h.append("</table>")
        h.append('<p class="note">The tolerance is %.4f. A class further than that '
                 "from every airframe in the weights is refused, and the hand "
                 "written tactics fly it. Nothing in the game says so, which is why "
                 "this is the first panel.</p>" % d.get("PILOT_SHIP_TOL", 0.0))
    else:
        h.append('<p class="note">No airframes. Build the PC target first.</p>')
    h.append("</section>")

    h.append('<section><h2>What it reads</h2><div class="bars">')
    for r in use:
        cls = "bad" if r["use"] < 0.08 else ("amber" if r["use"] > 0.6 else "")
        h.append('<div><span class="dim">%2d</span> <span class="%s">%-16s</span>'
                 '<span class="dim">%s</span> %.2f</div>'
                 % (r["index"], cls, r["name"][:16], bar(r["use"]), r["use"]))
    h.append("</div>")
    h.append('<p class="note">The size of the first layer weights on each input, '
             "against the largest. Every input is scaled to a common size before it "
             "reaches the layer, so this is a fair comparison. A field near the "
             "bottom is one the network does not use: either it carries nothing, or "
             "it means something different in flight than it did in training.</p>")
    h.append("</section>")

    if modes:
        h.append('<section><h2>Mode head</h2><table><tr><th>mode</th>'
                 '<th class="num">bias</th><th class="num">weight size</th></tr>')
        for nm, b, w in modes:
            h.append('<tr><td class="amber">%s</td><td class="num">%+.3f</td>'
                     '<td class="num">%.2f</td></tr>' % (nm, b, w))
        h.append("</table>")
        h.append('<p class="note">One output for each mode. The largest wins, and it '
                 "is held for VG_MODE_DWELL seconds before another can replace it. A "
                 "mode with a much smaller weight size than the rest is one the "
                 "network rarely chooses.</p></section>")

    if rec:
        h.append('<section><h2>Modes in the recorded flying</h2><div class="bars">')
        for nm, sh in zip(rec["names"], rec["share"]):
            h.append('<div><span class="amber">%-8s</span><span class="dim">%s</span>'
                     " %.1f%%</div>" % (nm, bar(sh), 100.0 * sh))
        h.append("</div>")
        h.append('<div class="grid" style="margin-top:14px">')
        for label, val in (("median hold", "%.2f s" % rec["median"]),
                           ("mean hold", "%.2f s" % rec["mean"]),
                           ("longest tenth", "%.2f s" % rec["p90"]),
                           ("under half a second", "%.0f%%" % (100.0 * rec["flicker"]))):
            h.append('<div class="stat"><b>%s</b><span>%s</span></div>' % (val, label))
        h.append("</div>")
        h.append('<p class="note">How long one mode lasts in the flying the labels '
                 "came from. A short hold means the labels flicker, and a network "
                 "fitted to them learns to flicker too.</p></section>")

    h.append("</div>")
    return "\n".join(h)


def main():
    global OBS
    ap = argparse.ArgumentParser(description="Look inside a trained pilot network.")
    ap.add_argument("dataset", nargs="*", help="dump files, to show mode statistics")
    ap.add_argument("--net", default=DEF_NET, help="the generated header to read")
    ap.add_argument("--out", default=DEF_OUT, help="where to write the HTML page")
    ap.add_argument("--horizon", type=int, default=60)
    ap.add_argument("--mode-smooth", type=int, default=91)
    args = ap.parse_args()

    if not os.path.exists(args.net):
        print("error: %s does not exist" % args.net)
        return 1
    text = read_text(args.net)
    d, a = defines(text), arrays(text)
    OBS = obs_names()

    gate = gate_check(a.get("PILOT_SHIP_SEEN", np.zeros(0)),
                      d.get("PILOT_SHIP_TOL", 0.0), live_airframes(),
                      d.get("PILOT_SHIP_N", 11))
    use = input_use(a["PILOT_W0"], d["PILOT_NET_IN"], OBS)

    modes = None
    n_mode = d.get("PILOT_MODE_N", 0)
    if n_mode:
        last = d.get("PILOT_NET_L", 2)
        lw = a.get("PILOT_W%d" % last)
        lb = a.get("PILOT_B%d" % last)
        if lw is not None and lb is not None:
            w = np.abs(lw.reshape(d["PILOT_NET_OUT"], -1)).sum(axis=1)
            names = mode_names()
            modes = [(names[k] if k < len(names) else "mode %d" % k,
                      float(lb[4 + k]), float(w[4 + k])) for k in range(n_mode)]

    rec = None
    if args.dataset:
        rec = mode_stats(args.dataset, args.horizon - 1, args.mode_smooth)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(render(d, a, gate, use, modes, rec, args.net))
    print("wrote %s" % args.out)
    for r in gate:
        if not r["ok"]:
            print("WARNING: the network refuses %s. The rules fly it." % r["name"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
