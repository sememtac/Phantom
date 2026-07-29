"""
Phantom Recorder -- records gameplay off the board at a true 60 fps.

Two steps, and the reason is arithmetic. A frame is 460,800 bytes and the link
carries 0.74 MB/s, so pixels cannot come off the board faster than about 23
frames a second. Sending them while you play stalls the game loop to 15 fps,
which is not a recording of this game -- it is a recording of a different,
worse game. So nothing is sent while you play.

  RECORD  logs the simulation instead of the picture: a frame duration and an
          input struct, 71 bytes a frame. The game runs at its true 60 fps.

  RENDER  re-runs that session on the device afterwards and pulls the real
          pixels back, as slowly as it likes.

The video is a genuine 60 fps because the frames really were 1/60 s apart when
they happened, and every pixel is the actual rasteriser output.

The capture runs on a worker thread and hands frames back through a queue: the
link stalls for hundreds of milliseconds at a time, and doing that on the UI
thread would freeze the window solid for the whole recording, which looks
identical to a crash.
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

# Shown in the title bar. Not decoration: a build that silently failed to
# rebuild is indistinguishable from one that did until you can read a version
# off the running window, and that already cost a round trip once.
VERSION = "2.0"

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
        self.session = None          # the recording waiting to be rendered
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

        # Two buttons, in the order they are used. Step two stays disabled until
        # there is something to render, so the workflow is not something the
        # window expects you to already know.
        self.btn_rec = ttk.Button(self, text="1. Record Gameplay",
                                  command=self._toggle_record)
        self.btn_rec.grid(row=3, column=0, columnspan=3, sticky="ew", padx=8, pady=(8, 2))

        # Gamma sits with Render because that is when it applies -- a session
        # holds no pixels, so the same recording can be rendered again at a
        # different setting without replaying anything.
        tk.Label(self, text="Gamma", fg=AMBER, bg=GROUND).grid(row=4, column=0,
                                                               sticky="e", **pad)
        self.gamma = tk.StringVar(value="1.0")
        tk.Entry(self, textvariable=self.gamma, width=6).grid(row=4, column=1,
                                                              sticky="w", **pad)
        tk.Label(self, text="1.0 = exact,  1.5 = panel-like", fg="#7a5a20",
                 bg=GROUND).grid(row=4, column=1, sticky="e", padx=8)

        self.btn_render = ttk.Button(self, text="2. Render to Video",
                                     command=self._render, state="disabled")
        self.btn_render.grid(row=5, column=0, columnspan=3, sticky="ew", padx=8, pady=2)

        self.bar = ttk.Progressbar(self, mode="determinate")
        self.bar.grid(row=6, column=0, columnspan=3, sticky="ew", padx=8, pady=2)

        self.status = tk.Label(self, text="ready -- press Record, then play",
                               fg=AMBER, bg=GROUND, anchor="w", width=1)
        self.status.grid(row=7, column=0, columnspan=3, sticky="ew", padx=8, pady=(2, 8))

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

    def _open_session(self):
        p = filedialog.askopenfilename(initialdir=self.out_dir.get(),
                                       filetypes=[("Phantom session", "*.phr")])
        if not p:
            return
        try:
            self.session = Session.load(p)
        except Exception as e:
            messagebox.showerror("Phantom Recorder", f"Could not open it:\n{e}")
            return
        self.btn_render.config(state="normal")
        self.status.config(text=f"{os.path.basename(p)} -- "
                                f"{len(self.session.frames)} frames, ready to render")

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
            self._reveal(os.path.dirname(self.last_output))
        else:
            messagebox.showinfo("Phantom Recorder", "Nothing recorded yet this session.")

    def _about(self):
        messagebox.showinfo(
            "About Phantom Recorder",
            "Records Phantom off the ESP32-S3 at a true 60 fps.\n\n"
            "It takes two steps because a frame is 460,800 bytes and the link\n"
            "carries 0.74 MB/s. Sending pixels while you play stalls the game\n"
            "to 15 fps, so nothing is sent while you play.\n\n"
            "RECORD logs the simulation instead -- a frame duration and an\n"
            "input struct, 71 bytes a frame. The game runs at its full speed.\n\n"
            "RENDER re-runs the session on the device afterwards and pulls the\n"
            "real pixels back. Expect about three minutes per minute of play.\n\n"
            "Recording RESTARTS the game, because a session has to begin\n"
            "somewhere the replay can also begin. Play from the menu.\n\n"
            "mp4 output needs ffmpeg on PATH; without it a PPM sequence is\n"
            "written instead.")

    def _busy(self):
        return self.worker is not None and self.worker.is_alive()

    def _port(self):
        return self._ports[self.port.current()]

    # -- record -------------------------------------------------------------

    def _toggle_record(self):
        if self._busy():
            self.stop_flag.set()
            self.status.config(text="stopping...")
            return
        if not self._ports:
            self.status.config(text="no serial ports found")
            return

        self._error = None
        self.session = None
        self.stop_flag.clear()
        self.btn_rec.config(text="Stop Recording")
        self.btn_render.config(state="disabled")
        self.bar.config(mode="indeterminate", value=0)
        self.bar.start(60)
        self.status.config(text="resetting the board...")

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
            self.q.put(("error", f"no response on {port} -- wrong port? "
                                 f"look for the one marked [ESP32]"))
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
            self.status.config(text="output folder does not exist")
            return

        try:
            g = float(self.gamma.get())
        except ValueError:
            self.status.config(text="gamma must be a number (1.0 is exact)")
            return
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
        try:
            reset_board(port)
            link = PhantomLink(port)
            link.open()
            link.replay_start(ses.hdr)
            writer = FrameWriter(out_dir, fps=round(fps, 3), fragmented=True)

            # One record always queued ahead. In strict lockstep the device sits
            # idle between frames and its final bytes do not leave the USB
            # driver until something else moves them -- the host then waits
            # eight seconds for a hundred bytes that arrive only once it gives
            # up and writes.
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
                    continue
                writer.write(rgb)
                done += 1
                self.q.put(("ren", done, len(ses.frames), time.time() - started,
                            subsample_ppm(rgb, w, h, PREVIEW)))
        except TimeoutError as e:
            self.q.put(("error", f"link stalled: {e}"))
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
                    # RAW PPM bytes, NOT base64. Tk 8.6 takes base64 for GIF and
                    # PNG only; its PPM handler wants the bytes themselves.
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
            # The pump must survive anything one message can do to it. It is the
            # only thing rescheduling itself, so an uncaught exception does not
            # drop a frame -- it stops the window updating for good.
            self.status.config(text=f"display error: {type(e).__name__}: {e}")
        self.after(60, self._pump)

    def _record_done(self, ses):
        self.bar.stop()
        self.bar.config(mode="determinate", value=0)
        self.btn_rec.config(text="1. Record Gameplay")

        if not ses or not ses.frames:
            if not self._error:
                self.status.config(text="nothing recorded -- is the board running?")
            return

        self.session = ses
        path = os.path.join(self.out_dir.get(),
                            time.strftime("phantom-%Y%m%d-%H%M%S.phr"))
        try:
            ses.save(path)
        except Exception as e:
            self.status.config(text=f"could not save the session: {e}")
            return

        fps = len(ses.frames) / max(0.001, ses.seconds)
        self.btn_render.config(state="normal")
        self.status.config(
            text=f"{len(ses.frames)} frames, {ses.seconds:.1f}s at {fps:.0f} fps"
                 f"  --  now press Render")

    def _render_done(self, done, path, fps, gamma):
        self.bar.stop()
        self.btn_rec.config(state="normal")
        self.btn_render.config(text="2. Render to Video")

        if not done:
            if not self._error:
                self.status.config(text="nothing rendered")
            return

        self.last_output = path
        self.status.config(
            text=f"{os.path.basename(path)}  --  {done} frames, "
                 f"{done / max(1.0, fps):.1f}s at {fps:.0f} fps"
                 + (f", gamma {gamma}" if gamma != 1.0 else ""))


if __name__ == "__main__":
    Recorder().mainloop()
