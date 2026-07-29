"""
Phantom Recorder -- a small window that pulls gameplay off the board.

Pick a port and an output folder, press Record, and it writes a timestamped mp4
(or a PPM sequence if ffmpeg is not on PATH) into that folder.

The capture runs on a worker thread and hands frames back through a queue: the
serial link stalls for hundreds of milliseconds at a time waiting on the device,
and doing that on the UI thread would freeze the window solid for the whole
recording, which looks identical to a crash.
"""

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
from phantom_link import (FPS, HEIGHT, WIDTH, Desync, FrameWriter, PhantomLink,
                          list_ports, subsample_ppm)

PREVIEW = 240        # pixels

# Shown in the title bar. Not decoration: a build that silently failed to
# rebuild is indistinguishable from one that did until you can read a version
# off the running window, and that already cost a round trip once.
VERSION = "1.3"

AMBER  = "#ffae1e"
GROUND = "#0d0700"


class Recorder(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"Phantom Recorder {VERSION}")
        self.resizable(False, False)
        self.configure(bg=GROUND)

        self.worker = None
        self.stop_flag = threading.Event()
        self.q = queue.Queue()
        self.frames = []
        self.last_output = None
        self._error = None
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

        self.sec_label = tk.Label(self, text="Seconds", fg=AMBER, bg=GROUND)
        self.sec_label.grid(row=3, column=0, sticky="e", **pad)
        self.sec_entry = tk.Entry(self, textvariable=self.seconds, width=6)
        self.sec_entry.grid(row=3, column=1, sticky="w", **pad)

        opts = dict(fg=AMBER, bg=GROUND, selectcolor=GROUND, activeforeground=AMBER,
                    activebackground=GROUND, highlightthickness=0, bd=0)

        self.cont = tk.BooleanVar(value=False)
        tk.Checkbutton(self, text="Continuous (record until Stop)",
                       variable=self.cont, command=self._mode_changed,
                       **opts).grid(row=4, column=0, columnspan=3, sticky="w", padx=8)

        # Live is what most people mean by "record": whatever is on the panel,
        # at the speed it actually happened. Smooth is the better-looking lie --
        # the firmware slows its own clock so no motion is missed between the
        # frames the link can carry.
        self.live = tk.BooleanVar(value=True)
        tk.Checkbutton(self, text="Live (real time, choppy)  --  off = smooth slow-mo",
                       variable=self.live, command=self._mode_changed,
                       **opts).grid(row=5, column=0, columnspan=3, sticky="w", padx=8)

        self.btn = ttk.Button(self, text="Record", command=self._toggle)
        self.btn.grid(row=6, column=0, columnspan=3, sticky="ew", padx=8, pady=(6, 2))

        self.bar = ttk.Progressbar(self, mode="determinate")
        self.bar.grid(row=7, column=0, columnspan=3, sticky="ew", padx=8, pady=2)

        self.status = tk.Label(self, text="idle", fg=AMBER, bg=GROUND,
                               anchor="w", width=1)
        self.status.grid(row=8, column=0, columnspan=3, sticky="ew", padx=8, pady=(2, 8))

        self._refresh()

    # -- helpers ------------------------------------------------------------

    def _refresh(self):
        ports = list_ports()
        self.port["values"] = [f"{d}  ({desc})" for d, desc in ports]
        if ports:
            self.port.current(0)
        self._ports = [d for d, _ in ports]

    def _mode_changed(self):
        self.sec_entry.config(state="disabled" if self.cont.get() else "normal")
        # The same box means two different things, so it says which. In fixed
        # mode a second of video is a known 30 frames and the wait is minutes; in
        # live mode a second of video IS a second of wall clock and the frame
        # count is whatever the link managed.
        self.sec_label.config(text="Seconds" if self.live.get() else "Video secs")

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
            "LIVE captures exactly what is on the panel, when it was on the\n"
            "panel. The game runs at its normal speed and you get whatever the\n"
            "link can carry -- a few frames a second, so the video is real time\n"
            "but choppy. This is the mode for recording yourself playing.\n\n"
            f"Turn Live OFF for SMOOTH: the firmware steps its simulation at a\n"
            f"fixed {FPS} fps however long each frame really takes, so the board\n"
            "runs in slow motion and the recording plays back perfectly smooth.\n"
            "Roughly five seconds of waiting per second of footage, and hard to\n"
            "play through -- best for menus, the bracket and the cutscene.\n\n"
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
        if not os.path.isdir(self.out_dir.get()):
            self.status.config(text="output folder does not exist")
            return

        # Fixed mode counts FRAMES to a target because it knows the rate; live
        # mode counts SECONDS on the clock because it does not. Asking live mode
        # for seconds*30 frames would record for as long as the link took to
        # deliver them, which is five times too long.
        live = self.live.get()
        want = want_secs = None
        if not self.cont.get():
            try:
                secs = float(self.seconds.get())
            except ValueError:
                self.status.config(text="seconds must be a number")
                return
            if live:
                want_secs = max(0.5, secs)
            else:
                want = max(1, int(secs * FPS))

        self._error = None
        self.stop_flag.clear()
        if want:
            self.bar.config(mode="determinate", maximum=want, value=0)
        elif want_secs:
            self.bar.config(mode="determinate", maximum=1000, value=0)
        else:
            # Nothing to be a fraction of, so the bar reports activity instead.
            self.bar.config(mode="indeterminate", value=0)
            self.bar.start(60)
        self.btn.config(text="Stop")

        # Everything the worker needs is read HERE, on the UI thread. Tk is not
        # thread-safe and a StringVar.get() from the worker reaches into the
        # interpreter from the wrong thread -- it happens to survive under
        # mainloop and raises "main thread is not in main loop" the moment
        # anything else drives the event loop.
        port = self._ports[self.port.current()]
        out_dir = self.out_dir.get()
        self.worker = threading.Thread(target=self._run,
                                       args=(port, out_dir, want, want_secs, live),
                                       daemon=True)
        self.worker.start()

    def _run(self, port, out_dir, want, want_secs, live):
        # Frames go straight to the writer as they arrive rather than into a
        # list. Continuous recording is otherwise impossible: at 691,200 bytes
        # of RGB888 per frame, an unbounded run fills memory long before it
        # fills a disk, and buffering would have capped a playthrough at a few
        # minutes for no reason other than how the fixed-length version happened
        # to be written.
        link = PhantomLink(port)
        writer = None
        started = time.time()
        try:
            # fps=None in live mode means the writer measures the real arrival
            # rate before opening the encoder, so the video plays at the speed
            # the game actually ran instead of a rate nobody knows in advance.
            writer = FrameWriter(out_dir,
                                 fps=None if live else FPS,
                                 fragmented=(want is None and want_secs is None))
            link.open()
            link.arm(live=live)
            while not self.stop_flag.is_set():
                elapsed = time.time() - started
                if want is not None and writer.n >= want:
                    break
                if want_secs is not None and elapsed >= want_secs:
                    break
                try:
                    rgb, w, h = link.read_frame()
                except Desync:
                    continue          # drop it and pick the stream back up
                writer.write(rgb)
                self.q.put(("frame", writer.n, want, want_secs,
                            time.time() - started, writer.fps,
                            subsample_ppm(rgb, w, h, PREVIEW)))
        except TimeoutError:
            # The specific failure worth naming. Everything else the user can
            # act on says what it is; this one used to surface as the generic
            # "no frames captured", which points at the board when the actual
            # fault is almost always the wrong port selected.
            self.q.put(("error", f"no response on {port} -- wrong port? "
                                 f"look for the one marked [ESP32]"))
        except Exception as e:
            self.q.put(("error", f"{type(e).__name__}: {e}"))
        finally:
            try:
                link.close()
            except Exception:
                pass
            path = writer.close() if writer else None
            n = writer.n if writer else 0
            self.q.put(("done", time.time() - started, n, path,
                        writer.fps if writer else FPS))

    # -- ui pump ------------------------------------------------------------

    def _pump(self):
        try:
            while True:
                msg = self.q.get_nowait()
                if msg[0] == "frame":
                    _, n, want, want_secs, elapsed, fps, ppm = msg
                    rate = n / max(0.001, elapsed)
                    if want:
                        self.bar["value"] = n
                        eta = (want - n) / max(0.01, rate)
                        self.status.config(
                            text=f"{n}/{want}   {rate:.1f}/s   ~{eta:.0f}s left")
                    elif want_secs:
                        # Live: the wait IS the recording, so count it down.
                        self.bar["value"] = min(1000, 1000 * elapsed / want_secs)
                        self.status.config(
                            text=f"{elapsed:.1f}/{want_secs:.1f}s   "
                                 f"{n} frames   {rate:.1f}/s")
                    else:
                        # Length of the VIDEO so far, which is the number that
                        # matters, not how long you have been sitting there.
                        self.status.config(
                            text=f"{n} frames   {n / max(1.0, fps):.1f}s of video"
                                 f"   {rate:.1f}/s")
                    # RAW PPM bytes, NOT base64. Tk 8.6 takes base64 for GIF and
                    # PNG only; its PPM handler wants the bytes themselves and
                    # rejects base64 with "couldn't recognize image data".
                    #
                    # That exception escaped _pump entirely, past the queue.Empty
                    # guard and past the after() that reschedules it -- so the
                    # pump died on the first frame every time and the window
                    # froze for the rest of the recording.
                    self._img = tk.PhotoImage(data=ppm)
                    if self._img_id is None:
                        self._img_id = self.canvas.create_image(0, 0, anchor="nw",
                                                                image=self._img)
                    else:
                        self.canvas.itemconfig(self._img_id, image=self._img)
                elif msg[0] == "error":
                    # Latched, so the "done" that follows cannot overwrite it.
                    # It did, which is how a wrong-port timeout was reported to
                    # the user as though the board had stopped running.
                    self._error = msg[1]
                    self.status.config(text=msg[1])
                elif msg[0] == "done":
                    self._finish(msg[1], msg[2], msg[3], msg[4])
        except queue.Empty:
            pass
        except Exception as e:
            # The pump must survive anything a single message can do to it. It
            # is the only thing rescheduling itself, so one uncaught exception
            # does not drop a frame -- it stops the window updating for good,
            # which is indistinguishable from the capture having hung.
            self.status.config(text=f"display error: {type(e).__name__}: {e}")
        self.after(60, self._pump)

    def _finish(self, elapsed, n, path, fps):
        self.bar.stop()
        self.bar.config(mode="determinate")
        self.btn.config(text="Record")

        if not n:
            self.bar["value"] = 0
            if not self._error:
                self.status.config(text="no frames captured -- is the board running?")
            return

        # Already written -- the file was being built the whole time, so there
        # is nothing to do here but say where it went.
        self.last_output = path
        self.bar.config(maximum=n, value=n)
        self.status.config(
            text=f"{os.path.basename(path)}  --  {n} frames, "
                 f"{n / max(1.0, fps):.1f}s @ {fps:.1f}fps")


if __name__ == "__main__":
    Recorder().mainloop()
