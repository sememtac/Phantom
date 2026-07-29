"""
Phantom Recorder -- a small window that pulls gameplay off the board.

Pick a port and an output folder, press Record, and it writes a timestamped mp4
(or a PPM sequence if ffmpeg is not on PATH) into that folder.

The capture runs on a worker thread and hands frames back through a queue: the
serial link stalls for hundreds of milliseconds at a time waiting on the device,
and doing that on the UI thread would freeze the window solid for the whole
recording, which looks identical to a crash.
"""

import base64
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import (FPS, HEIGHT, WIDTH, Desync, PhantomLink, list_ports,
                          subsample_ppm)

PREVIEW = 240        # pixels

AMBER  = "#ffae1e"
GROUND = "#0d0700"


class Recorder(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Phantom Recorder")
        self.resizable(False, False)
        self.configure(bg=GROUND)

        self.worker = None
        self.stop_flag = threading.Event()
        self.q = queue.Queue()
        self.frames = []
        self.last_output = None
        self.out_dir = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "Videos"))
        self.seconds = tk.StringVar(value="10")

        self._build_menu()
        self._build()
        self.after(60, self._pump)

    # -- menus --------------------------------------------------------------

    def _build_menu(self):
        bar = tk.Menu(self)

        f = tk.Menu(bar, tearoff=0)
        f.add_command(label="Choose Output Folder...", accelerator="Ctrl+O",
                      command=self._browse)
        f.add_command(label="Open Output Folder", command=self._open_folder)
        f.add_command(label="Show Last Recording", command=self._show_last)
        f.add_separator()
        f.add_command(label="Exit", accelerator="Alt+F4", command=self.destroy)
        bar.add_cascade(label="File", menu=f)

        c = tk.Menu(bar, tearoff=0)
        c.add_command(label="Record / Stop", accelerator="Ctrl+R", command=self._toggle)
        c.add_command(label="Refresh Ports", accelerator="F5", command=self._refresh)
        bar.add_cascade(label="Capture", menu=c)

        h = tk.Menu(bar, tearoff=0)
        h.add_command(label="About", command=self._about)
        bar.add_cascade(label="Help", menu=h)

        self.config(menu=bar)
        self.bind("<Control-o>", lambda e: self._browse())
        self.bind("<Control-r>", lambda e: self._toggle())
        self.bind("<F5>", lambda e: self._refresh())

    # -- layout -------------------------------------------------------------

    def _build(self):
        pad = dict(padx=8, pady=3)

        # A CANVAS, not a Label. Label width/height are in TEXT UNITS until an
        # image is attached, so asking a bare Label for width=240 requests a
        # widget 240 CHARACTERS wide -- which is how the first build opened at a
        # size that would not fit on a television. Canvas measures in pixels.
        self.canvas = tk.Canvas(self, width=PREVIEW, height=PREVIEW,
                                bg="black", highlightthickness=1,
                                highlightbackground="#4a2f08")
        self.canvas.grid(row=0, column=0, columnspan=3, padx=8, pady=(8, 4))
        self._img_id = None

        tk.Label(self, text="Port", fg=AMBER, bg=GROUND).grid(row=1, column=0, sticky="e", **pad)
        self.port = ttk.Combobox(self, width=22, state="readonly")
        self.port.grid(row=1, column=1, sticky="ew", **pad)
        ttk.Button(self, text="Refresh", width=8, command=self._refresh).grid(row=1, column=2, **pad)

        tk.Label(self, text="Folder", fg=AMBER, bg=GROUND).grid(row=2, column=0, sticky="e", **pad)
        tk.Entry(self, textvariable=self.out_dir, width=24).grid(row=2, column=1, sticky="ew", **pad)
        ttk.Button(self, text="Browse", width=8, command=self._browse).grid(row=2, column=2, **pad)

        tk.Label(self, text="Seconds", fg=AMBER, bg=GROUND).grid(row=3, column=0, sticky="e", **pad)
        tk.Entry(self, textvariable=self.seconds, width=6).grid(row=3, column=1, sticky="w", **pad)

        self.btn = ttk.Button(self, text="Record", command=self._toggle)
        self.btn.grid(row=4, column=0, columnspan=3, sticky="ew", padx=8, pady=(6, 2))

        self.bar = ttk.Progressbar(self, mode="determinate")
        self.bar.grid(row=5, column=0, columnspan=3, sticky="ew", padx=8, pady=2)

        self.status = tk.Label(self, text="idle", fg=AMBER, bg=GROUND,
                               anchor="w", width=1)
        self.status.grid(row=6, column=0, columnspan=3, sticky="ew", padx=8, pady=(2, 8))

        self._refresh()

    # -- helpers ------------------------------------------------------------

    def _refresh(self):
        ports = list_ports()
        self.port["values"] = [f"{d}  ({desc})" for d, desc in ports]
        if ports:
            self.port.current(0)
        self._ports = [d for d, _ in ports]

    def _browse(self):
        d = filedialog.askdirectory(initialdir=self.out_dir.get())
        if d:
            self.out_dir.set(d)

    def _reveal(self, path):
        if sys.platform.startswith("win"):
            os.startfile(path)                                   # noqa: S606
        elif sys.platform == "darwin":
            subprocess.Popen(["open", path])
        else:
            subprocess.Popen(["xdg-open", path])

    def _open_folder(self):
        d = self.out_dir.get()
        if os.path.isdir(d):
            self._reveal(d)
        else:
            messagebox.showwarning("Phantom Recorder", "That folder does not exist.")

    def _show_last(self):
        if self.last_output and os.path.exists(self.last_output):
            self._reveal(os.path.dirname(self.last_output) if os.path.isfile(self.last_output)
                         else self.last_output)
        else:
            messagebox.showinfo("Phantom Recorder", "Nothing recorded yet this session.")

    def _about(self):
        messagebox.showinfo(
            "About Phantom Recorder",
            "Records Phantom off the ESP32-S3 over USB.\n\n"
            f"Capture is not real time. While armed, the firmware steps its\n"
            f"simulation at a fixed {FPS} fps however long each frame takes, so the\n"
            "board runs slow and the recording plays back smooth.\n\n"
            "Expect roughly five seconds of waiting per second of footage.\n\n"
            "mp4 output needs ffmpeg on PATH; without it a PPM sequence is\n"
            "written instead.")

    # -- capture ------------------------------------------------------------

    def _toggle(self):
        if self.worker and self.worker.is_alive():
            # Stopping keeps whatever has already arrived. A recording cut short
            # is still a recording; throwing it away would punish the one action
            # the user takes when they have got the shot they wanted.
            self.stop_flag.set()
            self.status.config(text="stopping...")
            return

        if not self._ports:
            self.status.config(text="no serial ports found")
            return
        try:
            want = max(1, int(float(self.seconds.get()) * FPS))
        except ValueError:
            self.status.config(text="seconds must be a number")
            return
        if not os.path.isdir(self.out_dir.get()):
            self.status.config(text="output folder does not exist")
            return

        self.frames = []
        self.stop_flag.clear()
        self.bar["maximum"] = want
        self.bar["value"] = 0
        self.btn.config(text="Stop")

        port = self._ports[self.port.current()]
        self.worker = threading.Thread(target=self._run, args=(port, want), daemon=True)
        self.worker.start()

    def _run(self, port, want):
        link = PhantomLink(port)
        started = time.time()
        try:
            link.open()
            link.arm()
            while len(self.frames) < want and not self.stop_flag.is_set():
                try:
                    rgb, w, h = link.read_frame()
                except Desync:
                    continue          # drop it and pick the stream back up
                self.frames.append(rgb)
                self.q.put(("frame", len(self.frames), want,
                            time.time() - started, subsample_ppm(rgb, w, h, PREVIEW)))
        except Exception as e:
            self.q.put(("error", str(e)))
        finally:
            try:
                link.close()
            except Exception:
                pass
            self.q.put(("done", time.time() - started))

    # -- ui pump ------------------------------------------------------------

    def _pump(self):
        try:
            while True:
                msg = self.q.get_nowait()
                if msg[0] == "frame":
                    _, n, want, elapsed, ppm = msg
                    self.bar["value"] = n
                    rate = n / max(0.001, elapsed)
                    eta = (want - n) / max(0.01, rate)
                    self.status.config(
                        text=f"{n}/{want}   {rate:.1f}/s   ~{eta:.0f}s left")
                    self._img = tk.PhotoImage(data=base64.b64encode(ppm))
                    if self._img_id is None:
                        self._img_id = self.canvas.create_image(0, 0, anchor="nw",
                                                                image=self._img)
                    else:
                        self.canvas.itemconfig(self._img_id, image=self._img)
                elif msg[0] == "error":
                    self.status.config(text=f"error: {msg[1]}")
                elif msg[0] == "done":
                    self._finish(msg[1])
        except queue.Empty:
            pass
        self.after(60, self._pump)

    def _finish(self, elapsed):
        self.btn.config(text="Record")
        if not self.frames:
            self.status.config(text="no frames captured -- is the board running?")
            return

        stamp = time.strftime("%Y%m%d-%H%M%S")
        n = len(self.frames)
        self.status.config(text=f"{n} frames in {elapsed:.0f}s -- writing...")
        self.update_idletasks()

        if shutil.which("ffmpeg"):
            path = os.path.join(self.out_dir.get(), f"phantom-{stamp}.mp4")
            p = subprocess.Popen(
                ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                 "-s", f"{WIDTH}x{HEIGHT}", "-r", str(FPS), "-i", "-",
                 "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", path],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            for f in self.frames:
                p.stdin.write(f)
            p.stdin.close()
            p.wait()
            self.last_output = path
            self.status.config(text=f"wrote {os.path.basename(path)}  ({n} frames)")
        else:
            d = os.path.join(self.out_dir.get(), f"phantom-{stamp}")
            os.makedirs(d, exist_ok=True)
            for i, f in enumerate(self.frames):
                with open(os.path.join(d, f"f{i:05d}.ppm"), "wb") as fh:
                    fh.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
                    fh.write(f)
            self.last_output = d
            self.status.config(text=f"no ffmpeg -- wrote {n} PPMs to {os.path.basename(d)}")


if __name__ == "__main__":
    Recorder().mainloop()
