"""One window for the work on the pilot network.

The weights are made on the PC in a loop: record some flying, train, look at what
came out, measure it, put it in the tree, fly against it. Each step was its own
command line with its own arguments. This window puts them in one place and shows
the state of the network between them.

Run it with tools\\pilot.cmd, or with:

    python tools/pilot_console.py

It runs the other scripts. It does not repeat them. train_pilot.py stays the
trainer, inspect_pilot.py stays the inspector and pilot_eval.py stays the
measurement. This window only calls them, so there is one copy of each job.

THE GAME FLIES pilot_net.h, AND NOTHING ELSE. The weights are compiled into the
game. Train writes pilot_net_next.h, and the game does not see that file until
Deploy copies it over pilot_net.h and Build compiles it.

The exe is tools/dist/PilotConsole.exe. Keep it in the repository, because it
finds the repository from its own folder. To build the exe again, see
tools/README.md.
"""
import datetime
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import webbrowser

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# pilot_eval.py loads from the repository while the window runs, so PyInstaller
# cannot see what it imports. These lines put those modules in the exe.
import concurrent.futures  # noqa: F401
import json  # noqa: F401
import math  # noqa: F401
import re  # noqa: F401
import statistics  # noqa: F401
import struct  # noqa: F401


def frozen():
    """True in the exe that PyInstaller makes. False when this file runs as a script."""
    return getattr(sys, "frozen", False)


def find_root():
    """Find the top folder of the repository.

    When this file runs as a script, the top folder is the folder above tools/.
    The exe has no source file. PyInstaller unpacks the program into a temporary
    folder, and __file__ points to that folder. So in the exe, this function
    starts in the folder of the exe. It goes up one folder at a time until it
    finds platformio.ini.
    """
    if not frozen():
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    here = os.path.dirname(os.path.abspath(sys.executable))
    while True:
        if os.path.isfile(os.path.join(here, "platformio.ini")):
            return here
        up = os.path.dirname(here)
        if up == here:
            return os.getcwd()
        here = up


def find_python():
    """Find the Python that runs the trainer, the inspector and the measurement.

    When this file runs as a script, use the same Python. In the exe,
    sys.executable is the exe itself, so use the python on the PATH. That Python
    needs numpy for the inspector, and torch for the trainer.
    """
    if not frozen():
        return sys.executable
    return shutil.which("python")


ROOT = find_root()
PYTHON = find_python()
GENERATED = os.path.join(ROOT, "src", "vg", "generated")
NET = os.path.join(GENERATED, "pilot_net.h")
NEXT = os.path.join(GENERATED, "pilot_net_next.h")
EXE = os.path.join(ROOT, "host", "build", "phantom.exe")
CAPTURES = os.path.join(ROOT, "captures")
FLY_LOGS = os.path.join(CAPTURES, "ai_eval", "fly")
REPORT = os.path.join(ROOT, "pilot_report.html")

CLASSES = ["AEGIS", "LANCE", "CHARIOT", "BALLISTA"]
LEVELS = ["RAW", "JRN", "PRO", "VET", "ACE"]

# THE M1 RECORDING PLAN. See captures/ai_eval/PLAN.md. The presets below make the
# same command lines and the same file names as the plan. Change them together.
#
# Session A is the flying the network copies. LANCE flies against RAW, because a
# LANCE lock needs room: against stronger pilots the recorded pilot spends the
# session without a lock, and the network copies that.
M1_A_LEVEL = {0: 1, 1: 0, 2: 1, 3: 1}     # hull -> level: JRN, but RAW for LANCE
# Session B is the human baseline: PRO, and a fresh bout each time.
M1_B_LEVEL = 2
M1_B_BOUT = 180
# Minutes of live flight to record for each hull: the default of the target field.
M1_TARGET = {"A": "20", "B": "10"}

# The weapon rules changed until 2026-09-04 and are frozen since then. A recording
# made before that date shows flying under other rules.
RULES_FROZEN = datetime.datetime(2026, 9, 4).timestamp()

BG = "#111113"
PANEL = "#18181b"
INK = "#e8dcc4"
DIM = "#8a8377"
AMBER = "#ffb000"
BAD = "#ff6a4d"
OK = "#8fd07a"
MONO = ("Consolas", 10)

# A console program started from this window opens an empty console window,
# because this window has no console. This flag stops that. The output still
# comes through the pipe or goes to the file.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def tools_path(name):
    return os.path.join(ROOT, "tools", name)


def load_tool(name):
    """Load a script from tools/ as a module, from the repository on disk."""
    import importlib.util
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    spec = importlib.util.spec_from_file_location(name[:-3], tools_path(name))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def who_flew(name):
    """Who flew a recording, from its name: the author or the bot."""
    if name.startswith(("a_", "m1_")):
        return "you"
    if name.startswith("d_"):
        return "bot"
    return "?"


def obs_minutes(path):
    """Minutes of live flight in a recording. One row is one frame at 60 a second."""
    try:
        with open(path, "rb") as f:
            head = f.read(12)
        if len(head) < 12:
            return 0.0
        packed = int.from_bytes(head[4:8], "little")
        act_n = int.from_bytes(head[8:12], "little")
        row = 4 * ((packed & 0xFFFF) + act_n)
        return (os.path.getsize(path) - 12) / row / 3600.0 if row else 0.0
    except OSError:
        return 0.0


def on_controller(log):
    """True if the log of a sitting says that a game controller was connected."""
    try:
        with open(log, "rb") as f:
            raw = f.read()
    except OSError:
        return False
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        raw = raw.decode("utf-16", errors="replace").encode("utf-8")
    return b"xinput" in raw and b"connected" in raw


def bout_count(log):
    """How many 'bout' lines a log holds. None if there is no log."""
    if not os.path.exists(log):
        return None
    with open(log, "rb") as f:
        raw = f.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = raw.decode("utf-16", errors="replace")
    else:
        text = raw.decode("utf-8", errors="replace")
    return sum(1 for line in text.splitlines() if line.startswith("bout ")
               and not line.startswith("bout open"))


class Console(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Phantom - pilot network")
        self.configure(bg=BG)
        self.geometry("1180x760")
        self.q = queue.Queue()
        self.busy = False
        self.net_path = NET
        self.names = []      # the file name for each row of the recordings list
        self.gate = {}       # class name -> True if the network flies that class
        self._build()
        self.after(120, self._drain)
        self.refresh()

    # ---- layout ----------------------------------------------------------
    def _build(self):
        top = tk.Frame(self, bg=BG)
        top.pack(fill="x", padx=14, pady=(14, 8))
        tk.Label(top, text="PILOT NETWORK", bg=BG, fg=AMBER,
                 font=("Consolas", 13, "bold")).pack(side="left")
        self.lbl_net = tk.Label(top, text="", bg=BG, fg=DIM, font=MONO)
        self.lbl_net.pack(side="left", padx=12)

        body = tk.Frame(self, bg=BG)
        body.pack(fill="both", expand=True, padx=14)

        left = tk.Frame(body, bg=PANEL, highlightbackground="#2a2a2e",
                        highlightthickness=1)
        left.pack(side="left", fill="both", expand=False, ipadx=6, ipady=6)

        tk.Label(left, text="STATE", bg=PANEL, fg=DIM, font=MONO).pack(
            anchor="w", padx=10, pady=(8, 2))
        self.state = tk.Text(left, width=56, height=12, bg=PANEL, fg=INK,
                             font=MONO, relief="flat", wrap="none",
                             highlightthickness=0)
        self.state.pack(padx=10, pady=(0, 8))
        for tag, colour in (("bad", BAD), ("ok", OK), ("amber", AMBER), ("dim", DIM)):
            self.state.tag_config(tag, foreground=colour)

        tk.Label(left, text="RECORDINGS   who  rules  name  bouts in the log",
                 bg=PANEL, fg=DIM, font=MONO).pack(anchor="w", padx=10, pady=(4, 2))
        wrap = tk.Frame(left, bg=PANEL)
        wrap.pack(padx=10, pady=(0, 8), fill="both", expand=True)
        self.files = tk.Listbox(wrap, selectmode="extended", height=14, width=56,
                                bg="#0f0f11", fg=INK, font=MONO, relief="flat",
                                selectbackground="#3a3320", highlightthickness=0,
                                exportselection=False)
        self.files.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(wrap, command=self.files.yview)
        sb.pack(side="right", fill="y")
        self.files.config(yscrollcommand=sb.set)
        tk.Label(left, text="you = the author's flying. bot = a dump of the bot.\n"
                            "old = made before the weapon rules of 2026-09-04.",
                 bg=PANEL, fg=DIM, font=MONO, justify="left").pack(
            anchor="w", padx=10, pady=(0, 6))

        right = tk.Frame(body, bg=BG)
        right.pack(side="left", fill="both", expand=True, padx=(12, 0))

        # The jobs. One runs at a time, and these buttons wait for it.
        row = tk.Frame(right, bg=BG)
        row.pack(fill="x")
        self.buttons = []
        for text, fn in (("Inspect", self.do_inspect),
                         ("Train", self.do_train),
                         ("Evaluate", self.do_evaluate),
                         ("Deploy", self.do_deploy),
                         ("Build", self.do_build)):
            self.buttons.append(self._button(row, text, fn))

        opts = tk.Frame(right, bg=BG)
        opts.pack(fill="x", pady=(8, 2))
        self.epochs = self._field(opts, "epochs", "300", 6)
        self.horizon = self._field(opts, "horizon", "60", 5)
        self.hidden = self._field(opts, "hidden", "64", 5)
        self.layers = self._field(opts, "layers", "2", 4)
        self.smooth = self._field(opts, "mode smooth", "91", 5)

        ev = tk.Frame(right, bg=BG)
        ev.pack(fill="x", pady=(2, 8))
        tk.Label(ev, text="evaluate:", bg=BG, fg=DIM, font=MONO).pack(side="left")
        self.ev_enemy = self._choice(ev, "opponent", ["net", "tactics"], "net")
        self.ev_seeds = self._field(ev, "seeds", "1-4", 6)
        self.ev_minutes = self._field(ev, "minutes", "5", 4)

        # The game. These start the game in its own window and do not wait for
        # it, so they stay usable while a job runs.
        fly = tk.Frame(right, bg=BG)
        fly.pack(fill="x", pady=(4, 2))
        self._button(fly, "Fly", self.do_fly)
        self.fly_seat = self._choice(fly, "you", CLASSES, CLASSES[0])
        self.fly_opp = self._choice(fly, "against", CLASSES, CLASSES[2])
        self.fly_level = self._choice(fly, "level", LEVELS, "PRO")

        rec = tk.Frame(right, bg=BG)
        rec.pack(fill="x", pady=(2, 2))
        self._button(rec, "Record", self.do_record)
        self.rec_session = self._choice(
            rec, "M1 session", ["A imitation", "B baseline"], "A imitation",
            self.session_changed)
        self.rec_hull = self._choice(rec, "hull", CLASSES, CLASSES[0], self.show_plan)
        # THE TARGET IS FOR THE HULL, not for the sitting. The game shows the
        # running total in the corner and stops when the total gets there.
        rec2 = tk.Frame(right, bg=BG)
        rec2.pack(fill="x", pady=(2, 2))
        tk.Label(rec2, text="        ", bg=BG, font=MONO).pack(side="left")
        self.rec_target = self._field(rec2, "target minutes", M1_TARGET["A"], 5)
        self.rec_target.bind("<KeyRelease>", lambda _e: self.show_plan())
        self.rec_count = self._choice(rec2, "count earlier sittings",
                                      ["controller", "all"], "controller",
                                      self.show_plan)
        self.lbl_plan = tk.Label(right, text="", bg=BG, fg=DIM, font=MONO,
                                 anchor="w", justify="left")
        self.lbl_plan.pack(fill="x", pady=(0, 8))

        tk.Label(right, text="LOG", bg=BG, fg=DIM, font=MONO).pack(anchor="w")
        self.log = tk.Text(right, bg="#0f0f11", fg=INK, font=MONO, relief="flat",
                           wrap="none", highlightthickness=0)
        self.log.pack(fill="both", expand=True, pady=(2, 14))
        self.log.tag_config("bad", foreground=BAD)
        self.log.tag_config("amber", foreground=AMBER)
        self.log.tag_config("ok", foreground=OK)
        self.show_plan()

    def _button(self, parent, text, fn):
        b = tk.Button(parent, text=text, command=fn, bg="#26262b", fg=INK,
                      activebackground="#3a3320", activeforeground=AMBER,
                      font=MONO, relief="flat", padx=14, pady=6,
                      highlightthickness=0, borderwidth=0)
        b.pack(side="left", padx=(0, 6))
        return b

    def _field(self, parent, label, default, width):
        tk.Label(parent, text=label, bg=BG, fg=DIM, font=MONO).pack(side="left")
        v = tk.Entry(parent, width=width, bg="#0f0f11", fg=INK, font=MONO,
                     relief="flat", insertbackground=AMBER, highlightthickness=1,
                     highlightbackground="#2a2a2e")
        v.insert(0, default)
        v.pack(side="left", padx=(4, 14))
        return v

    def _choice(self, parent, label, values, default, changed=None):
        """A drop-down list. Give its variable."""
        tk.Label(parent, text=label, bg=BG, fg=DIM, font=MONO).pack(
            side="left", padx=(8, 0))
        var = tk.StringVar(value=default)
        m = tk.OptionMenu(parent, var, *values,
                          command=(lambda _v: changed()) if changed else None)
        m.config(bg="#26262b", fg=INK, activebackground="#3a3320",
                 activeforeground=AMBER, font=MONO, relief="flat",
                 highlightthickness=0, borderwidth=0)
        m["menu"].config(bg="#26262b", fg=INK, font=MONO,
                         activebackground="#3a3320", activeforeground=AMBER)
        m.pack(side="left", padx=(4, 4))
        return var

    # ---- running things --------------------------------------------------
    def say(self, text, tag=None):
        self.log.insert("end", text + "\n", tag or ())
        self.log.see("end")

    def _drain(self):
        while True:
            try:
                kind, payload = self.q.get_nowait()
            except queue.Empty:
                break
            if kind == "line":
                tag = "bad" if payload.lower().startswith(("error", "warning")) else None
                self.say(payload.rstrip(), tag)
            elif kind == "say":
                self.say(*payload)
            elif kind == "refresh":
                self.refresh()
                self.show_plan()
            elif kind == "done":
                label, then, lines = payload
                self.busy = False
                for b in self.buttons:
                    b.config(state="normal")
                self.say("-- %s --" % label, "amber")
                if then:
                    then(lines)
                self.refresh()
        self.after(120, self._drain)

    def run(self, cmd, done, cwd=ROOT, then=None):
        """Run a command and put its output in the log. One at a time.

        `then` gets the lines of output when the command stops.
        """
        if self.busy:
            self.say("A job runs now. Wait for it to stop.", "bad")
            return
        self.busy = True
        for b in self.buttons:
            b.config(state="disabled")
        self.say("$ " + " ".join(os.path.basename(c) for c in cmd[:3]) + " ...", "amber")

        def worker():
            env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
            lines = []
            try:
                p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", env=env,
                                     creationflags=NO_WINDOW)
                for line in p.stdout:
                    lines.append(line)
                    self.q.put(("line", line))
                p.wait()
            except Exception as exc:
                self.q.put(("line", "error: %s" % exc))
            self.q.put(("done", (done, then, lines)))

        threading.Thread(target=worker, daemon=True).start()

    def play(self, args, log, what):
        """Start the game in its own window. Write its output to `log`.

        This does not wait for the game. When the window closes, the log shows
        the bouts that were flown.
        """
        if not os.path.exists(EXE):
            self.say("There is no phantom.exe. Push Build first.", "bad")
            return
        if self.busy:
            self.say("A job runs now. The game can run slowly until it stops.", "bad")
        os.makedirs(os.path.dirname(log), exist_ok=True)
        cmd = [EXE] + args
        self.say("$ phantom.exe " + " ".join(args), "amber")
        self.say("output to %s" % os.path.relpath(log, ROOT))
        try:
            f = open(log, "wb")
            p = subprocess.Popen(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                                 creationflags=NO_WINDOW)
        except OSError as exc:
            self.say("error: %s" % exc, "bad")
            return

        def watch():
            p.wait()
            f.close()
            self.q.put(("say", ("-- %s stopped --" % what, "amber")))
            try:
                r = load_tool("pilot_eval.py").score_log(log)
                dec = r["kills"] + r["deaths"]
                self.q.put(("say", (
                    "%.1f min: you won %d, the opponent won %d, %d draws%s"
                    % (r["minutes"], r["kills"], r["deaths"], r.get("draws", 0),
                       ("  (opponent %.0f%%)" % (100.0 * r["deaths"] / dec)) if dec else ""),
                    "ok")))
            except Exception as exc:
                self.q.put(("say", ("no bouts to count: %s" % exc, None)))
            # Tk is not safe from a second thread, so the main loop does this.
            self.q.put(("refresh", None))

        threading.Thread(target=watch, daemon=True).start()

    def chosen(self):
        return [os.path.join("captures", self.names[i])
                for i in self.files.curselection()]

    def have_python(self):
        if PYTHON:
            return True
        self.say("Cannot find Python. Install Python 3 and put it on the PATH.", "bad")
        return False

    # ---- the state panel -------------------------------------------------
    def refresh(self):
        shown = os.path.relpath(self.net_path, ROOT)
        if os.path.abspath(self.net_path) == os.path.abspath(NET):
            self.lbl_net.config(text="%s  (the game flies this file)" % shown)
        else:
            self.lbl_net.config(text="%s  (the game flies pilot_net.h, not this file)"
                                % shown)
        self.state.config(state="normal")
        self.state.delete("1.0", "end")
        self.state.insert("end", "python  ", "dim")
        self.state.insert("end", "%s\n" % (PYTHON or "NOT FOUND"),
                          None if PYTHON else "bad")
        self.state.insert("end", "repo    ", "dim")
        self.state.insert("end", "%s\n" % ROOT)
        try:
            ip = load_tool("inspect_pilot.py")
            text = ip.read_text(self.net_path)
            d, a = ip.defines(text), ip.arrays(text)
            n_mode = d.get("PILOT_MODE_N", 0)
            self.state.insert("end", "shape   ", "dim")
            self.state.insert("end", "%d - %d x %d - %d\n"
                              % (d.get("PILOT_NET_IN", 0), d.get("PILOT_NET_H", 0),
                                 d.get("PILOT_NET_L", 2), d.get("PILOT_NET_OUT", 0)))
            self.state.insert("end", "modes   ", "dim")
            self.state.insert("end", "%s\n" % (n_mode or "none"),
                              "amber" if n_mode else "dim")
            gate = ip.gate_check(a.get("PILOT_SHIP_SEEN"), d.get("PILOT_SHIP_TOL", 0),
                                 ip.live_airframes(), d.get("PILOT_SHIP_N", 11))
            self.state.insert("end", "gate\n", "dim")
            if os.path.abspath(self.net_path) == os.path.abspath(NET):
                self.gate = {r["name"]: r["ok"] for r in gate}
            for r in gate:
                self.state.insert("end", "  %-9s " % r["name"])
                self.state.insert("end", "%s  %.4f\n"
                                  % ("flown" if r["ok"] else "REFUSED", r["dist"]),
                                  "ok" if r["ok"] else "bad")
            if not gate:
                self.state.insert("end", "  build the PC target first\n", "dim")
        except Exception as exc:
            self.state.insert("end", "cannot read the network:\n%s\n" % exc, "bad")
        self.state.config(state="disabled")

        keep = set(self.names[i] for i in self.files.curselection()
                   if i < len(self.names))
        self.files.delete(0, "end")
        self.names = []
        if os.path.isdir(CAPTURES):
            for f in sorted(x for x in os.listdir(CAPTURES) if x.endswith(".obs")):
                path = os.path.join(CAPTURES, f)
                old = os.path.getmtime(path) < RULES_FROZEN
                n = bout_count(path[:-4] + ".log")
                logtxt = "-" if n is None else "%d" % n
                self.files.insert("end", "%-3s  %-3s  %-34s %s"
                                  % (who_flew(f), "old" if old else "", f[:34], logtxt))
                self.names.append(f)
                if who_flew(f) == "bot":
                    self.files.itemconfig(len(self.names) - 1, fg=DIM)
                if f in keep:
                    self.files.selection_set(len(self.names) - 1)

    def show_plan(self):
        """Show the command line that Record will use, before it runs."""
        args, obs, log = self.m1_plan(peek=True)
        ctrl, every = self.recorded()
        self.lbl_plan.config(
            text="Record: phantom.exe %s\n        output to %s\n"
                 "        %s, session %s: %.1f min recorded on the controller, "
                 "%.1f min in all"
                 % (" ".join(args), os.path.relpath(log, ROOT), self.rec_hull.get(),
                    self.rec_session.get()[0], ctrl, every))

    def session_changed(self):
        """Put the default target of the session in the target field."""
        self.rec_target.delete(0, "end")
        self.rec_target.insert(0, M1_TARGET[self.rec_session.get()[0]])
        self.show_plan()

    def recorded(self):
        """Minutes already recorded for the hull and session that are selected.

        Give (on the controller, in all). A sitting is on the controller if its log
        says that a controller was connected.
        """
        import re
        lower = self.rec_hull.get().lower()
        if self.rec_session.get().startswith("A"):
            level = LEVELS[M1_A_LEVEL[CLASSES.index(self.rec_hull.get())]].lower()
            pat = re.compile(r"^m1_%s_%s_\d+\.obs$" % (lower, level))
        else:
            pat = re.compile(r"^m1_%s_pro(_\d+)?\.obs$" % lower)
        ctrl = every = 0.0
        if os.path.isdir(CAPTURES):
            for f in os.listdir(CAPTURES):
                if not pat.match(f):
                    continue
                m = obs_minutes(os.path.join(CAPTURES, f))
                every += m
                if on_controller(os.path.join(CAPTURES, f[:-4] + ".log")):
                    ctrl += m
        return ctrl, every

    def target(self):
        """The target in minutes from the field, or 0 if the field is not a number."""
        try:
            return max(0.0, float(self.rec_target.get()))
        except ValueError:
            return 0.0

    # ---- the buttons -----------------------------------------------------
    def do_inspect(self):
        if not self.have_python():
            return
        cmd = [PYTHON, tools_path("inspect_pilot.py"), "--net", self.net_path]
        cmd += self.chosen()
        cmd += ["--mode-smooth", self.smooth.get(), "--horizon", self.horizon.get()]
        self.run(cmd, "inspected")
        self.after(1500, lambda: webbrowser.open("file:///" + REPORT.replace("\\", "/")))

    def do_train(self):
        if not self.have_python():
            return
        data = self.chosen()
        if not data:
            self.say("Select some recordings first.", "bad")
            return
        names = [os.path.basename(p) for p in data]
        bots = [n for n in names if who_flew(n) == "bot"]
        if bots and not messagebox.askyesno(
                "Bot recordings selected",
                "These recordings are the bot's flying, not yours:\n\n%s\n\n"
                "The network copies the flying it is trained on. "
                "Train on them anyway?" % "\n".join(bots), default="no"):
            return
        old = [n for n in names
               if os.path.getmtime(os.path.join(CAPTURES, n)) < RULES_FROZEN]
        if old and not messagebox.askyesno(
                "Old recordings selected",
                "These recordings were made before the weapon rules of 2026-09-04:"
                "\n\n%s\n\nTrain on them anyway?" % "\n".join(old), default="no"):
            return
        out = filedialog.asksaveasfilename(
            title="Where to write the weights", defaultextension=".h",
            initialdir=GENERATED, initialfile=os.path.basename(NEXT))
        if not out:
            return
        if os.path.abspath(out) == os.path.abspath(NET):
            self.say("Train does not write pilot_net.h. Write another file, "
                     "then use Deploy.", "bad")
            return
        cmd = [PYTHON, tools_path("train_pilot.py")] + data + [
            "--epochs", self.epochs.get(), "--horizon", self.horizon.get(),
            "--hidden", self.hidden.get(), "--layers", self.layers.get(),
            "--mode-smooth", self.smooth.get(), "--out", out]

        def shown(_lines):
            if os.path.exists(out):
                self.net_path = out
                self.say("The state panel shows %s. The game still flies "
                         "pilot_net.h." % os.path.basename(out))

        self.run(cmd, "trained", then=shown)

    def do_evaluate(self):
        """Measure the opponents with pilot_eval.py, and show the table."""
        if not self.have_python():
            return
        if not os.path.exists(EXE):
            self.say("There is no phantom.exe. Push Build first.", "bad")
            return
        name = time.strftime("console-%Y%m%d-%H%M%S")
        cmd = [PYTHON, tools_path("pilot_eval.py"), "run", "--name", name,
               "--enemy", self.ev_enemy.get(), "--seeds", self.ev_seeds.get(),
               "--minutes", self.ev_minutes.get()]
        self.say("The evaluation flies the weights that are compiled into "
                 "phantom.exe. At the default settings it takes about 2 minutes.")
        self.run(cmd, "evaluated", then=self.show_table)

    def show_table(self, lines):
        """Show the table of an evaluation in its own window."""
        start = next((i for i, l in enumerate(lines)
                      if l.startswith("seat ")), None)
        if start is None:
            self.say("The evaluation gave no table. Look at the log.", "bad")
            return
        win = tk.Toplevel(self)
        win.title("Evaluation")
        win.configure(bg=BG)
        t = tk.Text(win, bg="#0f0f11", fg=INK, font=MONO, relief="flat",
                    wrap="none", width=118, height=min(48, len(lines) - start + 2))
        t.pack(fill="both", expand=True, padx=10, pady=10)
        t.insert("end", "".join(lines[start:]))
        t.config(state="disabled")

    def do_deploy(self):
        src = filedialog.askopenfilename(
            title="Which weights to put in the game", filetypes=[("header", "*.h")],
            initialdir=GENERATED, initialfile=os.path.basename(NEXT))
        if not src:
            return
        if os.path.abspath(src) != os.path.abspath(NET):
            if not messagebox.askyesno(
                    "Replace the live weights?",
                    "This REPLACES\n\n    %s\n\nwith\n\n    %s\n\n"
                    "The game flies the new weights after the build. "
                    "git keeps the old file.\n\nReplace pilot_net.h?"
                    % (os.path.relpath(NET, ROOT), os.path.relpath(src, ROOT)),
                    icon="warning", default="no"):
                self.say("Deploy stopped. pilot_net.h did not change.")
                return
            shutil.copyfile(src, NET)
            self.say("copied %s over pilot_net.h" % os.path.basename(src))
        self.net_path = NET
        self.do_build()

    def do_build(self):
        # Windows does not run PowerShell scripts by default. "-ExecutionPolicy
        # Bypass" runs build.ps1 one time and changes no setting. See "Windows
        # blocks the PowerShell scripts at first" in tools/README.md.
        self.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                  os.path.join(ROOT, "host", "build.ps1")], "built")

    def do_fly(self):
        """Fly against an opponent that the network flies."""
        seat = CLASSES.index(self.fly_seat.get())
        opp = CLASSES.index(self.fly_opp.get())
        level = LEVELS.index(self.fly_level.get())
        name = self.fly_opp.get()
        if self.gate and not self.gate.get(name, False):
            self.say("The network refuses %s, so the hand-written AI flies it. "
                     "This fight does not show the network." % name, "bad")
        else:
            self.say("The network flies %s for part of each fight. The panel "
                     "names the layer that flies it: NET is the network." % name)
        args = ["--scale", "3", "--gym", str(seat), str(opp), "--pilot", str(level),
                "--enemy-net", "--show-ai"]
        log = os.path.join(FLY_LOGS, time.strftime("fly-%Y%m%d-%H%M%S.log"))
        self.play(args, log, "the fight")

    def m1_plan(self, peek=False):
        """The command line, recording and log for the M1 session that is selected.

        With peek, give the next free name and ask nothing.
        """
        hull = CLASSES.index(self.rec_hull.get())
        lower = CLASSES[hull].lower()
        if self.rec_session.get().startswith("A"):
            level = M1_A_LEVEL[hull]
            n = 1
            while (os.path.exists(os.path.join(CAPTURES, "m1_%s_%s_%d.obs"
                                               % (lower, LEVELS[level].lower(), n)))
                   or os.path.exists(os.path.join(CAPTURES, "m1_%s_%s_%d.log"
                                                  % (lower, LEVELS[level].lower(), n)))):
                n += 1
            stem = "m1_%s_%s_%d" % (lower, LEVELS[level].lower(), n)
            args = ["--scale", "3", "--gym", str(hull), "0", "--rotate",
                    "--pilot", str(level), "--dump", "captures\\%s.obs" % stem]
        else:
            stem = "m1_%s_pro" % lower
            if not peek and (os.path.exists(os.path.join(CAPTURES, stem + ".obs"))
                             or os.path.exists(os.path.join(CAPTURES, stem + ".log"))):
                ans = messagebox.askyesnocancel(
                    "The baseline exists",
                    "%s.obs or %s.log exists.\n\n"
                    "Yes: replace it.\nNo: keep it and record a second sitting "
                    "as %s_2.\nCancel: record nothing." % (stem, stem, stem))
                if ans is None:
                    return None
                if ans is False:
                    stem += "_2"
            args = ["--scale", "3", "--gym", str(hull), "0", "--rotate",
                    "--pilot", str(M1_B_LEVEL), "--bouts", str(M1_B_BOUT),
                    "--dump", "captures\\%s.obs" % stem]
        # THE CLOCK. Added at the end, so the flags of the plan come first and stay
        # the same. The game counts its own rows on top of what is already done.
        goal = self.target()
        if goal > 0.0:
            ctrl, every = self.recorded()
            done = ctrl if self.rec_count.get() == "controller" else every
            args += ["--record-minutes", "%g" % goal, "--record-done", "%.3f" % done]
        return args, os.path.join(CAPTURES, stem + ".obs"), \
            os.path.join(CAPTURES, stem + ".log")

    def do_record(self):
        """Record one M1 sitting, with the command line from the plan."""
        plan = self.m1_plan()
        if plan is None:
            return
        args, obs, log = plan
        goal = self.target()
        if goal > 0.0:
            ctrl, every = self.recorded()
            done = ctrl if self.rec_count.get() == "controller" else every
            if done >= goal:
                self.say("%s session %s has %.1f of %.0f minutes already. To record "
                         "more, set a larger target." % (self.rec_hull.get(),
                         self.rec_session.get()[0], done, goal), "ok")
                return
            self.say("The game stops by itself at %.0f minutes. The clock in the top "
                     "right shows the total. Pauses and menus do not count." % goal)
        if self.rec_session.get().startswith("B"):
            self.say("Session B: each bout restarts the moment it ends. You do not "
                     "see the kill or your wreck. Count it from the .log only.")
        self.say("Stay in the fight. Do not go to the menus or the course. "
                 "You can close the game window to stop early.")
        self.play(args, log, "the recording")
        self.after(500, self.show_plan)


if __name__ == "__main__":
    Console().mainloop()
