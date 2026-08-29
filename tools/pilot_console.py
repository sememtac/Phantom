"""One window for the work on the pilot network.

The weights are made on the PC in a loop: record some flying, train, look at what
came out, put it in the tree, fly it. Each step was its own command line with its
own arguments. This puts them in one place and shows the state of the network
between them.

Run it with tools\\pilot.cmd, or with:

    python tools/pilot_console.py

It runs the other scripts rather than repeating them. train_pilot.py stays the
trainer and inspect_pilot.py stays the inspector; this window only calls them, so
there is one copy of each job.
"""
import os
import queue
import subprocess
import sys
import threading
import webbrowser

import tkinter as tk
from tkinter import filedialog, ttk

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(ROOT, "src", "vg", "generated", "pilot_net.h")
EXE = os.path.join(ROOT, "host", "build", "phantom.exe")
CAPTURES = os.path.join(ROOT, "captures")
REPORT = os.path.join(ROOT, "pilot_report.html")

BG = "#111113"
PANEL = "#18181b"
INK = "#e8dcc4"
DIM = "#8a8377"
AMBER = "#ffb000"
BAD = "#ff6a4d"
OK = "#8fd07a"
MONO = ("Consolas", 10)


def tools_path(name):
    return os.path.join(ROOT, "tools", name)


class Console(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Phantom - pilot network")
        self.configure(bg=BG)
        self.geometry("980x680")
        self.q = queue.Queue()
        self.busy = False
        self.net_path = NET
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
        self.state = tk.Text(left, width=44, height=12, bg=PANEL, fg=INK,
                             font=MONO, relief="flat", wrap="none",
                             highlightthickness=0)
        self.state.pack(padx=10, pady=(0, 8))
        self.state.tag_config("bad", foreground=BAD)
        self.state.tag_config("ok", foreground=OK)
        self.state.tag_config("amber", foreground=AMBER)
        self.state.tag_config("dim", foreground=DIM)

        tk.Label(left, text="RECORDINGS", bg=PANEL, fg=DIM, font=MONO).pack(
            anchor="w", padx=10, pady=(4, 2))
        wrap = tk.Frame(left, bg=PANEL)
        wrap.pack(padx=10, pady=(0, 8), fill="both", expand=True)
        self.files = tk.Listbox(wrap, selectmode="extended", height=13, width=44,
                                bg="#0f0f11", fg=INK, font=MONO, relief="flat",
                                selectbackground="#3a3320", highlightthickness=0,
                                exportselection=False)
        self.files.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(wrap, command=self.files.yview)
        sb.pack(side="right", fill="y")
        self.files.config(yscrollcommand=sb.set)

        right = tk.Frame(body, bg=BG)
        right.pack(side="left", fill="both", expand=True, padx=(12, 0))

        row = tk.Frame(right, bg=BG)
        row.pack(fill="x")
        self.buttons = []
        for text, fn in (("Inspect", self.do_inspect),
                         ("Train", self.do_train),
                         ("Deploy", self.do_deploy),
                         ("Fly", self.do_fly),
                         ("Record", self.do_record),
                         ("Build", self.do_build)):
            b = tk.Button(row, text=text, command=fn, bg="#26262b", fg=INK,
                          activebackground="#3a3320", activeforeground=AMBER,
                          font=MONO, relief="flat", padx=14, pady=6,
                          highlightthickness=0, borderwidth=0)
            b.pack(side="left", padx=(0, 6))
            self.buttons.append(b)

        opts = tk.Frame(right, bg=BG)
        opts.pack(fill="x", pady=(8, 6))
        self.epochs = self._field(opts, "epochs", "300", 6)
        self.horizon = self._field(opts, "horizon", "60", 5)
        self.hidden = self._field(opts, "hidden", "64", 5)
        self.layers = self._field(opts, "layers", "2", 4)
        self.smooth = self._field(opts, "mode smooth", "91", 5)

        tk.Label(right, text="LOG", bg=BG, fg=DIM, font=MONO).pack(anchor="w")
        self.log = tk.Text(right, bg="#0f0f11", fg=INK, font=MONO, relief="flat",
                           wrap="none", highlightthickness=0)
        self.log.pack(fill="both", expand=True, pady=(2, 14))
        self.log.tag_config("bad", foreground=BAD)
        self.log.tag_config("amber", foreground=AMBER)

    def _field(self, parent, label, default, width):
        tk.Label(parent, text=label, bg=BG, fg=DIM, font=MONO).pack(side="left")
        v = tk.Entry(parent, width=width, bg="#0f0f11", fg=INK, font=MONO,
                     relief="flat", insertbackground=AMBER, highlightthickness=1,
                     highlightbackground="#2a2a2e")
        v.insert(0, default)
        v.pack(side="left", padx=(4, 14))
        return v

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
            elif kind == "done":
                self.busy = False
                for b in self.buttons:
                    b.config(state="normal")
                self.say("-- %s --" % payload, "amber")
                self.refresh()
        self.after(120, self._drain)

    def run(self, cmd, done, cwd=ROOT):
        """Run a command and put its output in the log. One at a time."""
        if self.busy:
            self.say("busy", "bad")
            return
        self.busy = True
        for b in self.buttons:
            b.config(state="disabled")
        self.say("$ " + " ".join(os.path.basename(c) for c in cmd[:3]) + " ...", "amber")

        def worker():
            env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
            try:
                p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", env=env)
                for line in p.stdout:
                    self.q.put(("line", line))
                p.wait()
            except Exception as exc:
                self.q.put(("line", "error: %s" % exc))
            self.q.put(("done", done))

        threading.Thread(target=worker, daemon=True).start()

    def chosen(self):
        return [os.path.join("captures", self.files.get(i))
                for i in self.files.curselection()]

    # ---- the state panel -------------------------------------------------
    def refresh(self):
        self.lbl_net.config(text=os.path.relpath(self.net_path, ROOT))
        self.state.config(state="normal")
        self.state.delete("1.0", "end")
        try:
            sys.path.insert(0, os.path.join(ROOT, "tools"))
            import importlib.util
            spec = importlib.util.spec_from_file_location(
                "ip", tools_path("inspect_pilot.py"))
            ip = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(ip)
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

        keep = set(self.files.get(i) for i in self.files.curselection())
        self.files.delete(0, "end")
        if os.path.isdir(CAPTURES):
            names = sorted(f for f in os.listdir(CAPTURES) if f.endswith(".obs"))
            for i, f in enumerate(names):
                self.files.insert("end", f)
                if f in keep:
                    self.files.selection_set(i)

    # ---- the buttons -----------------------------------------------------
    def do_inspect(self):
        cmd = [sys.executable, tools_path("inspect_pilot.py"), "--net", self.net_path]
        cmd += self.chosen()
        cmd += ["--mode-smooth", self.smooth.get(), "--horizon", self.horizon.get()]
        self.run(cmd, "inspected")
        self.after(1500, lambda: webbrowser.open("file:///" + REPORT.replace("\\", "/")))

    def do_train(self):
        data = self.chosen()
        if not data:
            self.say("choose some recordings first", "bad")
            return
        out = filedialog.asksaveasfilename(
            title="where to write the weights", defaultextension=".h",
            initialdir=os.path.join(ROOT, "src", "vg", "generated"),
            initialfile="pilot_net.h")
        if not out:
            return
        cmd = [sys.executable, tools_path("train_pilot.py")] + data + [
            "--epochs", self.epochs.get(), "--horizon", self.horizon.get(),
            "--hidden", self.hidden.get(), "--layers", self.layers.get(),
            "--mode-smooth", self.smooth.get(), "--out", out]
        self.run(cmd, "trained")

    def do_deploy(self):
        src = filedialog.askopenfilename(
            title="which weights to put in the tree", filetypes=[("header", "*.h")],
            initialdir=os.path.join(ROOT, "src", "vg", "generated"))
        if not src:
            return
        if os.path.abspath(src) != os.path.abspath(NET):
            import shutil
            shutil.copyfile(src, NET)
            self.say("copied %s into the tree" % os.path.basename(src))
        self.net_path = NET
        self.do_build()

    def do_build(self):
        self.run(["powershell", "-NoProfile", "-File",
                  os.path.join(ROOT, "host", "build.ps1")], "built")

    def do_fly(self):
        if not os.path.exists(EXE):
            self.say("no phantom.exe. Press Build first.", "bad")
            return
        subprocess.Popen([EXE, "--scale", "3"], cwd=ROOT)
        self.say("flying")

    def do_record(self):
        if not os.path.exists(EXE):
            self.say("no phantom.exe. Press Build first.", "bad")
            return
        out = filedialog.asksaveasfilename(
            title="where to write the recording", defaultextension=".obs",
            initialdir=CAPTURES, initialfile="a_new.obs")
        if not out:
            return
        subprocess.Popen([EXE, "--scale", "3", "--dump", out], cwd=ROOT)
        self.say("recording to %s" % os.path.basename(out))


if __name__ == "__main__":
    Console().mainloop()
