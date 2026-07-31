"""
Phantom Recorder. This program records the game from the device at 60 fps.

There are two steps because of the link. One frame is 460,800 bytes and the link
carries 0.74 MB/s, so the device cannot send more than about 23 frames each
second. If it sends frames while you play, the game loop slows to 15 fps, and
the video then shows a slower game than the game you play. For this reason the
device sends no pixels while you play.

  RECORD  saves the simulation, not the picture. Each frame needs one frame
          time and one input structure, which is 71 bytes. The game keeps its
          full speed of 60 fps.

  RENDER  runs that session again on the device and reads the true pixels. This
          step is slow, and that is acceptable.

The video is 60 fps because the game made the frames 1/60 s apart. Every pixel
is the output of the rasteriser.

A worker thread does the link work and sends the frames to the window through a
queue. The link stops for some hundred milliseconds at a time. On the thread of
the window this would stop the window for the full record, and a user cannot
tell that condition from a crash.
"""

import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import (Desync, FrameWriter, PhantomLink, Session,
                          list_ports, reset_board, set_gamma, subsample_ppm)

PREVIEW = 240        # pixels

# The title bar shows this version. It is necessary: if a build fails, the old
# program stays on disk and looks the same. The version in the window is the
# only proof that the new build runs. This error already cost one cycle.
VERSION = "2.1"

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
        self.last_output = None
        self.session = None          # the session that waits for a render
        self._error = None
        self.out_dir = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "Videos"))

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
        f.add_command(label="Open Session...", command=self._open_session)
        f.add_separator()
        f.add_command(label="Exit", accelerator="Alt+F4", command=self.destroy)
        bar.add_cascade(label="File", menu=f)

        c = tk.Menu(bar, tearoff=0)
        c.add_command(label="Record / Stop", accelerator="Ctrl+R", command=self._toggle_record)
        c.add_command(label="Render to Video", accelerator="Ctrl+D", command=self._render)
        c.add_command(label="Refresh Ports", accelerator="F5", command=self._refresh)
        bar.add_cascade(label="Capture", menu=c)

        h = tk.Menu(bar, tearoff=0)
        h.add_command(label="About", command=self._about)
        bar.add_cascade(label="Help", menu=h)

        self.config(menu=bar)
        self.bind("<Control-o>", lambda e: self._browse())
        self.bind("<Control-r>", lambda e: self._toggle_record())
        self.bind("<Control-d>", lambda e: self._render())
        self.bind("<F5>", lambda e: self._refresh())

    # -- layout -------------------------------------------------------------

    def _build(self):
        pad = dict(padx=8, pady=3)

        # Use a Canvas, not a Label. A Label measures its width and its height
        # in TEXT UNITS until an image is attached. A Label with width=240 is
        # therefore 240 CHARACTERS wide, and the first build opened at a size
        # larger than a television screen. A Canvas measures in pixels.
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

        # The two buttons are in the order of use. Button two stays disabled
        # until a session exists, so the window shows the order of the steps.
        self.btn_rec = ttk.Button(self, text="1. Record Gameplay",
                                  command=self._toggle_record)
        self.btn_rec.grid(row=3, column=0, columnspan=3, sticky="ew", padx=8, pady=(8, 2))

        # Gamma is near the Render button because the render step applies it. A
        # session holds no pixels, so you can render the same session again at a
        # different gamma. The device does not run the session again.
        #
        # The value has its own COLUMN. The earlier entry box shared column 1
        # with its hint label. Two widgets in one cell made the box very small.
        tk.Label(self, text="Gamma", fg=AMBER, bg=GROUND).grid(row=4, column=0,
                                                               sticky="e", **pad)
        # Use ttk.Scale, not tk.Scale. A tk.Scale draws its THUMB in the
        # background colour of the widget. The background of this window is
        # nearly black, so the thumb was not visible and the control looked like
        # a gap in the trough. A ttk.Scale uses the theme of the platform and
        # draws a thumb that the user can see and move. A ttk.Scale ignores bg
        # and fg, so it does not use the amber colours. The ttk buttons above it
        # have the same appearance.
        self.gamma = tk.DoubleVar(value=1.0)
        ttk.Scale(self, from_=1.0, to=2.0, orient="horizontal",
                  variable=self.gamma, command=self._gamma_changed
                  ).grid(row=4, column=1, sticky="ew", padx=8)
        self.gamma_lbl = tk.Label(self, text="1.00", fg=AMBER, bg=GROUND, width=8)
        self.gamma_lbl.grid(row=4, column=2, sticky="w")

        tk.Label(self, text="1.0 keeps the exact values.  1.5 is near the panel.",
                 fg="#7a5a20", bg=GROUND, anchor="w", width=1
                 ).grid(row=5, column=0, columnspan=3, sticky="ew", padx=10)

        self.btn_render = ttk.Button(self, text="2. Render to Video",
                                     command=self._render, state="disabled")
        self.btn_render.grid(row=6, column=0, columnspan=3, sticky="ew", padx=8, pady=(4, 2))

        self.bar = ttk.Progressbar(self, mode="determinate")
        self.bar.grid(row=7, column=0, columnspan=3, sticky="ew", padx=8, pady=2)

        self.status = tk.Label(self, text="Ready. Press Record, then play the game.",
                               fg=AMBER, bg=GROUND, anchor="w", width=1)
        self.status.grid(row=8, column=0, columnspan=3, sticky="ew", padx=8, pady=(2, 8))

        self._refresh()

    # -- helpers ------------------------------------------------------------

    def _refresh(self):
        ports = list_ports()
        self.port["values"] = [f"{d}  ({desc})" for d, desc in ports]
        if ports:
            self.port.current(0)
        self._ports = [d for d, _ in ports]

    def _gamma_changed(self, value=None):
        # ttk.Scale is continuous, so snap here. Only write back when the
        # rounded value actually differs, or setting the variable inside its own
        # callback re-enters this endlessly.
        raw = self.gamma.get() if value is None else float(value)
        g = round(raw / 0.05) * 0.05
        if abs(g - self.gamma.get()) > 1e-9:
            self.gamma.set(g)
        self.gamma_lbl.config(text=f"{g:.2f}")

    def _browse(self):
        d = filedialog.askdirectory(initialdir=self.out_dir.get())
        if d:
            self.out_dir.set(d)

    def _open_session(self):
        p = filedialog.askopenfilename(initialdir=self.out_dir.get(),
                                       filetypes=[("Phantom session", "*.phr")])
        if not p:
            return
        try:
            self.session = Session.load(p)
        except Exception as e:
            messagebox.showerror("Phantom Recorder", f"The tool could not open the file:\n{e}")
            return
        self.btn_render.config(state="normal")
        self.status.config(text=f"{os.path.basename(p)}: "
                                f"{len(self.session.frames)} frames. Ready to render.")

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
            messagebox.showwarning("Phantom Recorder", "The folder does not exist.")

    def _show_last(self):
        if self.last_output and os.path.exists(self.last_output):
            self._reveal(os.path.dirname(self.last_output))
        else:
            messagebox.showinfo("Phantom Recorder", "This session has no recording yet.")

    def _about(self):
        messagebox.showinfo(
            "About Phantom Recorder",
            "This program records Phantom from the ESP32-S3 at 60 fps.\n\n"
            "There are two steps because of the link. One frame is 460,800\n"
            "bytes and the link carries 0.74 MB/s. If the device sends pixels\n"
            "while you play, the game slows to 15 fps. For this reason the\n"
            "device sends no pixels while you play.\n\n"
            "RECORD saves the simulation. Each frame needs one frame time and\n"
            "one input structure, which is 71 bytes. The game keeps its full\n"
            "speed.\n\n"
            "RENDER runs the session again on the device and reads the true\n"
            "pixels. This step takes about three minutes for each minute of\n"
            "play.\n\n"
            "A record restarts the game. A session must start at a state that\n"
            "the render step can also start from. Play from the menu.\n\n"
            "To write mp4 files you need ffmpeg on the PATH. Without ffmpeg\n"
            "the program writes a sequence of PPM files.")

    def _busy(self):
        return self.worker is not None and self.worker.is_alive()

    def _port(self):
        return self._ports[self.port.current()]

    # -- record -------------------------------------------------------------

    def _toggle_record(self):
        if self._busy():
            self.stop_flag.set()
            self.status.config(text="The tool stops...")
            return
        if not self._ports:
            self.status.config(text="No serial port was found.")
            return

        self._error = None
        self.session = None
        self.stop_flag.clear()
        self.btn_rec.config(text="Stop Recording")
        self.btn_render.config(state="disabled")
        self.bar.config(mode="indeterminate", value=0)
        self.bar.start(60)
        self.status.config(text="The device resets...")

        self.worker = threading.Thread(target=self._run_record,
                                       args=(self._port(),), daemon=True)
        self.worker.start()

    def _run_record(self, port):
        link = None
        ses = None
        try:
            reset_board(port)
            link = PhantomLink(port)
            link.open()
            ses = Session(link.session_start())
            started = time.time()
            while not self.stop_flag.is_set():
                ses.frames.append(link.session_frame())
                if len(ses.frames) % 15 == 0:
                    self.q.put(("rec", len(ses.frames), ses.seconds,
                                time.time() - started))
        except TimeoutError:
            self.q.put(("error", f"{port} did not respond. "
                                 f"Select the port marked [ESP32]."))
        except Exception as e:
            self.q.put(("error", f"{type(e).__name__}: {e}"))
        finally:
            if link:
                try:
                    link.session_end()
                except Exception:
                    pass
                link.close()
            self.q.put(("rec_done", ses))

    # -- render -------------------------------------------------------------

    def _render(self):
        if self._busy() or not self.session:
            return
        if not os.path.isdir(self.out_dir.get()):
            self.status.config(text="The output folder does not exist.")
            return

        # A slider cannot produce a value that needs validating, which is most of
        # why it is one.
        g = self.gamma.get()
        set_gamma(g)

        self._error = None
        self.stop_flag.clear()
        self.btn_rec.config(state="disabled")
        self.btn_render.config(text="Stop Rendering")
        n = len(self.session.frames)
        self.bar.config(mode="determinate", maximum=n, value=0)

        self.worker = threading.Thread(
            target=self._run_render,
            args=(self._port(), self.out_dir.get(), self.session, g), daemon=True)
        self.worker.start()

    def _run_render(self, port, out_dir, ses, gamma):
        fps = len(ses.frames) / max(0.001, ses.seconds)
        link = None
        writer = None
        done = 0
        misses = 0
        try:
            reset_board(port)
            link = PhantomLink(port)
            link.open()
            link.replay_start(ses.hdr)
            writer = FrameWriter(out_dir, fps=round(fps, 3), fragmented=True)

            # The host always sends one entry more than the device asked for.
            # A strict exchange of one entry for one frame makes the device idle
            # between frames. The last bytes of a frame then stay in the USB
            # driver until more traffic moves them. The host waited eight
            # seconds for 100 bytes.
            depth = 2
            for fr in ses.frames[:depth]:
                link.replay_send(fr)

            started = time.time()
            for i, fr in enumerate(ses.frames):
                if self.stop_flag.is_set():
                    break
                if i + depth < len(ses.frames):
                    link.replay_send(ses.frames[i + depth])
                try:
                    rgb, w, h = link.read_frame()
                except Desync:
                    # A frame that the tool cannot read used to be skipped
                    # without a message. If the device sends a block that this
                    # build is older than, EVERY frame is skipped. The only
                    # symptom is a progress bar that does not move, which looks
                    # like a slow render and is not one. This has cost two
                    # sessions. A render that reads no frames at all is not
                    # slow. It is too old for the firmware.
                    misses += 1
                    if done == 0 and misses >= 30:
                        self.q.put(("error",
                                    "The device sends data that this build of "
                                    "the tool cannot read. Build the tool "
                                    "again from tools/."))
                        break
                    continue
                writer.write(rgb)
                writer.add_audio(link.audio)
                link.audio = bytearray()
                done += 1
                self.q.put(("ren", done, len(ses.frames), time.time() - started,
                            subsample_ppm(rgb, w, h, PREVIEW)))
        except TimeoutError as e:
            self.q.put(("error", f"The link stopped: {e}"))
        except Exception as e:
            self.q.put(("error", f"{type(e).__name__}: {e}"))
        finally:
            if link:
                try:
                    link.session_end()
                except Exception:
                    pass
                link.close()
            path = writer.close() if writer else None
            self.q.put(("ren_done", done, path, fps, gamma))

    # -- ui pump ------------------------------------------------------------

    def _pump(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]

                if kind == "rec":
                    _, n, secs, el = msg
                    self.status.config(
                        text=f"recording   {n} frames   {secs:.1f}s of play   "
                             f"{n / max(0.001, secs):.0f} fps")

                elif kind == "rec_done":
                    self._record_done(msg[1])

                elif kind == "ren":
                    _, done, total, el, ppm = msg
                    rate = done / max(0.001, el)
                    self.bar["value"] = done
                    self.status.config(
                        text=f"rendering   {done}/{total}   {rate:.1f}/s   "
                             f"~{(total - done) / max(0.01, rate):.0f}s left")
                    # Give Tk the RAW PPM bytes, NOT base64. Tk 8.6 accepts
                    # base64 for GIF and for PNG only. Its PPM reader needs the
                    # bytes.
                    self._img = tk.PhotoImage(data=ppm)
                    if self._img_id is None:
                        self._img_id = self.canvas.create_image(0, 0, anchor="nw",
                                                                image=self._img)
                    else:
                        self.canvas.itemconfig(self._img_id, image=self._img)

                elif kind == "ren_done":
                    self._render_done(msg[1], msg[2], msg[3], msg[4])

                elif kind == "error":
                    # Latched, so the "done" that follows cannot overwrite it.
                    self._error = msg[1]
                    self.status.config(text=msg[1])
        except queue.Empty:
            pass
        except Exception as e:
            # The pump must survive every error that one message can cause.
            # The pump is the only function that starts itself again. An error
            # that is not caught does not lose one frame. It stops all updates
            # of the window.
            self.status.config(text=f"display error: {type(e).__name__}: {e}")
        self.after(60, self._pump)

    def _record_done(self, ses):
        self.bar.stop()
        self.bar.config(mode="determinate", value=0)
        self.btn_rec.config(text="1. Record Gameplay")

        if not ses or not ses.frames:
            if not self._error:
                self.status.config(text="No frames arrived. Make sure the device is on.")
            return

        self.session = ses
        path = os.path.join(self.out_dir.get(),
                            time.strftime("phantom-%Y%m%d-%H%M%S.phr"))
        try:
            ses.save(path)
        except Exception as e:
            self.status.config(text=f"The tool could not save the session: {e}")
            return

        fps = len(ses.frames) / max(0.001, ses.seconds)
        self.btn_render.config(state="normal")
        self.status.config(
            text=f"{len(ses.frames)} frames, {ses.seconds:.1f} s at {fps:.0f} fps."
                 f"  Now press Render.")

    def _render_done(self, done, path, fps, gamma):
        self.bar.stop()
        self.btn_rec.config(state="normal")
        self.btn_render.config(text="2. Render to Video")

        if not done:
            if not self._error:
                self.status.config(text="No frames were rendered.")
            return

        self.last_output = path
        self.status.config(
            text=f"{os.path.basename(path)}: {done} frames, "
                 f"{done / max(1.0, fps):.1f} s at {fps:.0f} fps"
                 + (f", gamma {gamma:.2f}" if gamma != 1.0 else ""))


if __name__ == "__main__":
    Recorder().mainloop()
