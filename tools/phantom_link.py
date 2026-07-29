"""
Device link for Phantom frame capture.

The wire protocol and the pixel conversions live here rather than in either
front end, so the CLI and the recorder cannot drift apart. Both bugs found in
the first version -- scanning for the frame magic inside binary payloads, and
copying the firmware's rotation instead of inverting it -- were the kind that
get fixed in one copy and left in the other.
"""

import os
import struct
import threading
import time

import serial
import serial.tools.list_ports

# numpy turns the per-frame pixel work from the bottleneck into a rounding
# error. Decoding runs and rotating 230,400 pixels in a Python loop costs
# roughly 150ms a frame, which capped capture at 5 fps while the wire was
# happily delivering 22. Vectorised it is a few milliseconds.
#
# Optional on purpose: the pure-Python path below is kept and still correct, so
# a machine without numpy records slowly rather than not at all.
try:
    import numpy as _np
except ImportError:
    _np = None

FPS = 60            # what the game targets, and what a session records at
WIDTH = HEIGHT = 480


ESPRESSIF_VID = 0x303A


def list_ports():
    """Boards first, ranked by USB VENDOR ID rather than by description text.

    Matching on strings looked reasonable and picked the wrong device: an FTDI
    adapter enumerating as "USB Serial Port (COM3)" scores identically to the
    board's "USB Serial Device (COM6)" against a substring like "usb serial",
    and sorts ahead of it on port number. The recorder then opened a port with
    nothing on the other end, waited for a header that was never coming, and
    reported it as though the board were not running.

    The vendor ID is not a guess. 0x303A is Espressif's, and nothing else on a
    normal machine claims it.
    """
    ports = list(serial.tools.list_ports.comports())

    def score(p):
        if p.vid == ESPRESSIF_VID:
            return 0
        blob = f"{p.description} {p.manufacturer or ''}".lower()
        if "esp32" in blob or "espressif" in blob:
            return 1
        return 2 if p.vid else 3          # anything with a VID beats Bluetooth

    ports.sort(key=score)
    out = []
    for p in ports:
        label = p.description or p.device
        if p.vid == ESPRESSIF_VID:
            label += "  [ESP32]"
        out.append((p.device, label))
    return out


class Desync(Exception):
    pass


def reset_board(port, settle=3.0):
    """Pulse the board's reset line and wait for it to come back up.

    Replay is a lockstep conversation, so a host that dies mid-session leaves
    the device waiting for a frame record and reading everything else as one.
    It recovers on its own after 30 seconds, which is a long time to look
    broken. Starting from a known state is cheaper than detecting every way the
    previous run could have ended -- and both operations reinitialise the game
    anyway, so nothing is lost by it.
    """
    s = serial.Serial(port, 115200)
    try:
        s.dtr = False
        s.rts = True
        time.sleep(0.15)
        s.rts = False
    finally:
        s.close()
    time.sleep(settle)


class PhantomLink:
    def __init__(self, port, baud=115200, timeout=8):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self._synced = False
        self._blob = 0          # size of the device's input struct, from the header

    # -- connection ---------------------------------------------------------

    def open(self):
        # DTR/RTS deasserted BEFORE the port opens, or opening it resets the
        # board. That is harmless for capture, which just re-arms, and quietly
        # fatal for replay: the 'P' lands while the device is booting, is lost,
        # and the frame records that follow are then read as commands -- one of
        # which is another 'P', which starts a replay from whatever bytes
        # happen to come next.
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baud
        self.ser.timeout = self.timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()

        # Windows gives a serial port a ~4KB receive buffer. A frame is 35KB
        # arriving in one burst, and the host does real work between bands
        # (decoding runs, writing into the frame), so the driver buffer
        # overflows and DROPS bytes -- silently, with no error anywhere. The
        # symptom is a frame that stops mid-band while the device, which sent
        # every byte, sits waiting for the next request.
        try:
            self.ser.set_buffer_size(rx_size=1 << 20, tx_size=1 << 16)
        except Exception:
            pass          # Windows-only API; elsewhere the default is ample

        time.sleep(0.4)
        self.ser.reset_input_buffer()
        self._start_reader()

    # -- receive thread -----------------------------------------------------
    #
    # Draining the port is separated from parsing it, because they cannot share
    # a thread without losing data. A frame arrives as a 35KB burst while the
    # parser is decoding runs and filling a numpy array, and every millisecond
    # spent doing that is a millisecond the driver's receive buffer is not being
    # emptied. It overflows and discards, silently -- measured at 3.6KB missing
    # from 1.6MB, against a device that reported writing every byte with no
    # short writes and no stalls.
    #
    # So this thread does nothing but move bytes out of the driver as fast as
    # they appear. Parsing then runs against a buffer in memory, where being
    # slow costs latency instead of data.

    def _start_reader(self):
        self._rx = bytearray()
        self._rx_lock = threading.Lock()
        self._rx_err = None
        self._rx_stop = False
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

    def _reader_loop(self):
        while not self._rx_stop:
            try:
                n = self.ser.in_waiting
                chunk = self.ser.read(n if n else 1)
            except Exception as e:
                self._rx_err = e
                return
            if chunk:
                with self._rx_lock:
                    self._rx += chunk

    def _rx_clear(self):
        with self._rx_lock:
            del self._rx[:]

    def close(self):
        self._rx_stop = True
        if self.ser:
            try:
                self.ser.write(b"s")
                self.ser.flush()
            except Exception:
                pass
            self.ser.close()
            self.ser = None

    # -- session record / replay -------------------------------------------
    #
    # The session is opaque here on purpose. The host stores the device's input
    # blobs and hands them back byte for byte without ever interpreting them, so
    # VgInput can gain a field without this file knowing. The device checks the
    # size on playback and refuses a mismatch, which is the one thing the host
    # could not detect.

    def session_start(self):
        """Begin recording a session. Returns the header to store with it."""
        self._rx_clear()
        self.ser.write(b"R")
        self.ser.flush()
        self._scan_to(b"PHRH")
        ver, blob, nr = struct.unpack("<HHB", self._read_exact(5))
        seeds = self._read_exact(nr * 4)
        save = self._read_exact(12)
        self._blob = blob
        return {"ver": ver, "blob": blob, "seeds": seeds, "save": save}

    def session_frame(self):
        """One recorded frame, or None if the device stopped."""
        tag = self._read_exact(4)
        if tag != b"PHRC":
            raise Desync(f"expected a frame record, got {tag!r}")
        idx, dt, nr = struct.unpack("<IfB", self._read_exact(9))
        seeds = self._read_exact(nr * 4)
        blob = self._read_exact(self._blob)
        return {"i": idx, "dt": dt, "seeds": seeds, "input": blob}

    def session_end(self):
        self.ser.write(b"E")
        self.ser.flush()

    def replay_start(self, hdr):
        self._rx_clear()
        self.ser.write(b"P")
        self.ser.write(struct.pack("<HHB", hdr["ver"], hdr["blob"],
                                   len(hdr["seeds"]) // 4))
        self.ser.write(hdr["seeds"])
        self.ser.write(hdr["save"])
        self.ser.flush()
        self._synced = False
        self._blob = hdr["blob"]

        # Wait to be told it is ready rather than sleeping a guessed interval.
        # The device re-initialises the game first, which regenerates a sky and
        # takes a few hundred milliseconds -- and if a frame record arrives
        # during that, it is consumed as a command instead.
        deadline = time.time() + 10.0
        seen = bytearray()
        while time.time() < deadline:
            try:
                seen += self._read_exact(1)
            except TimeoutError:
                break
            if b"vg_replay: PLAYING" in seen:
                return
            if b"vg_replay: REJECT" in seen:
                raise Desync(
                    "device refused the session -- it was recorded by a "
                    "different firmware build")
        raise TimeoutError("device never started the replay")

    def replay_send(self, fr):
        self.ser.write(b"PHRP")
        self.ser.write(struct.pack("<fB", fr["dt"], len(fr["seeds"]) // 4))
        self.ser.write(fr["seeds"])
        self.ser.write(fr["input"])
        self.ser.flush()

    # -- stream -------------------------------------------------------------

    def _read_exact(self, n):
        """Serial reads come back short constantly and a frame is fifteen bands;
        every one of them will be split. Looping here rather than at each call
        site is the difference between working and appearing to work."""
        deadline = time.time() + self.timeout
        while True:
            with self._rx_lock:
                if len(self._rx) >= n:
                    out = bytes(self._rx[:n])
                    del self._rx[:n]
                    return out
                have = len(self._rx)
            if self._rx_err:
                raise self._rx_err
            if time.time() > deadline:
                raise TimeoutError(f"link went quiet with {n - have} bytes outstanding")
            time.sleep(0.0005)

    def _scan_to(self, magic):
        window = bytearray()
        while True:
            b = self._read_exact(1)
            window += b
            if len(window) > 4:
                del window[0]
            if bytes(window) == magic:
                return

    def read_frame(self):
        """One frame as RGB888 bytes, upright. Raises Desync if the stream is
        lost mid-frame; the caller can simply ask for the next one."""
        # Sync ONCE, then read tags positionally.
        #
        # Scanning before every frame looks more robust and is the opposite:
        # band payloads are arbitrary binary, so the four bytes 'PHFR' turn up
        # inside compressed pixel data soon enough and the scanner locks onto
        # one, treating the middle of a band as a header.
        if not self._synced:
            self._scan_to(b"PHFR")
            self._synced = True
        else:
            tag = self._read_exact(4)
            if tag != b"PHFR":
                self._scan_to(b"PHFR")

        _idx, w, h, rot, fmt = struct.unpack("<IHHBB", self._read_exact(10))
        if fmt != 1:
            raise Desync(f"unknown pixel format {fmt}")

        pixels = _np.zeros(w * h, dtype=_np.uint16) if _np is not None else [0] * (w * h)
        while True:
            tag = self._read_exact(4)
            if tag == b"PHEN":
                self._read_exact(4)
                break
            if tag != b"PHBD":
                self._synced = False
                raise Desync("lost the stream mid-frame")
            y, bh, nbytes = struct.unpack("<HHI", self._read_exact(8))
            pixels[y * w:(y + bh) * w] = _decode_rle(self._read_exact(nbytes), w * bh)

        return to_rgb(pixels, w, h, rot), w, h


def _decode_rle(payload, npix):
    """u8 run, u16 pixel, little-endian."""
    if _np is not None:
        a = _np.frombuffer(payload, dtype=_np.uint8)
        a = a[:(len(a) // 3) * 3].reshape(-1, 3)
        vals = a[:, 1].astype(_np.uint16) | (a[:, 2].astype(_np.uint16) << 8)
        out = _np.repeat(vals, a[:, 0].astype(_np.int32))
        if out.size != npix:
            fixed = _np.zeros(npix, dtype=_np.uint16)
            fixed[:min(npix, out.size)] = out[:npix]
            out = fixed
        return out

    out = []
    i, n = 0, len(payload)
    while i + 2 < n:
        out.extend([payload[i + 1] | (payload[i + 2] << 8)] * payload[i])
        i += 3
    if len(out) != npix:
        out = (out + [0] * npix)[:npix]
    return out


# 5/6/5 -> 8/8/8, built once. Per-pixel shifting in Python is the single
# slowest thing in the capture path, and a frame is 230,400 pixels.
_R5 = [(v * 255) // 31 for v in range(32)]
_G6 = [(v * 255) // 63 for v in range(64)]

if _np is not None:
    _R5_LUT = _np.array(_R5, dtype=_np.uint8)
    _G6_LUT = _np.array(_G6, dtype=_np.uint8)


def to_rgb(pixels, w, h, rot):
    """RGB565 in PANEL byte order and PANEL orientation to upright RGB888.

    Both conversions have to happen somewhere. The device stores pixels
    pre-swapped so a band blit is a straight DMA, and never un-rotates because
    the panel is mounted a quarter turn off -- doing either on the board would
    cost frame time purely to save the host a loop.
    """
    if _np is not None and isinstance(pixels, _np.ndarray):
        return _to_rgb_np(pixels, w, h, rot)

    rgb = bytearray(w * h * 3)

    # This is the INVERSE of the firmware's rot_pt, not a copy of it. Getting
    # that backwards is a 180 degree error for rot 1 and 3, which looks exactly
    # like a correct capture of an upside-down game.
    #
    #   rot 1: firmware maps logical (lx,ly) -> panel (ly, H-1-lx)
    #   rot 3: firmware maps logical (lx,ly) -> panel (W-1-ly, lx)
    for i, v in enumerate(pixels):
        v = ((v & 0xFF) << 8) | (v >> 8)          # panel order -> native
        x, y = i % w, i // w
        if rot == 1:      sx, sy = (h - 1 - y), x
        elif rot == 2:    sx, sy = (w - 1 - x), (h - 1 - y)
        elif rot == 3:    sx, sy = y, (w - 1 - x)
        else:             sx, sy = x, y
        o = (sy * w + sx) * 3
        rgb[o]     = _R5[(v >> 11) & 0x1F]
        rgb[o + 1] = _G6[(v >> 5) & 0x3F]
        rgb[o + 2] = _R5[v & 0x1F]
    return bytes(rgb)


class Session:
    """A recorded session: a header and a list of per-frame records.

    Small enough to keep in memory and to keep forever -- a ten minute session
    at 60fps is 36,000 records of under a hundred bytes, so about three
    megabytes. That is the whole trick: the thing worth storing was never the
    pixels.
    """

    MAGIC = b"PHRS"

    def __init__(self, hdr=None):
        self.hdr = hdr
        self.frames = []

    @property
    def seconds(self):
        return sum(f["dt"] for f in self.frames)

    def save(self, path):
        with open(path, "wb") as fh:
            fh.write(self.MAGIC)
            fh.write(struct.pack("<HHBI", self.hdr["ver"], self.hdr["blob"],
                                 len(self.hdr["seeds"]) // 4, len(self.frames)))
            fh.write(self.hdr["seeds"])
            fh.write(self.hdr["save"])
            for f in self.frames:
                fh.write(struct.pack("<fB", f["dt"], len(f["seeds"]) // 4))
                fh.write(f["seeds"])
                fh.write(f["input"])
        return path

    @classmethod
    def load(cls, path):
        with open(path, "rb") as fh:
            if fh.read(4) != cls.MAGIC:
                raise ValueError(f"{os.path.basename(path)} is not a Phantom session")
            ver, blob, nr, nframes = struct.unpack("<HHBI", fh.read(9))
            hdr = {"ver": ver, "blob": blob, "seeds": fh.read(nr * 4),
                   "save": fh.read(12)}
            s = cls(hdr)
            for _ in range(nframes):
                dt, fnr = struct.unpack("<fB", fh.read(5))
                s.frames.append({"dt": dt, "seeds": fh.read(fnr * 4),
                                 "input": fh.read(blob)})
        return s


def _to_rgb_np(pixels, w, h, rot):
    """Vectorised twin of the loop in to_rgb. Same inverse rotation, derived
    rather than copied:

        firmware rot 1 maps logical (lx,ly) -> panel (ly, H-1-lx), so the panel
        array S undoes it as D[x, h-1-y] = S[y, x], which is S transposed with
        its columns reversed. rot 3 is the same transpose flipped the other way,
        rot 2 is both axes reversed.

    Getting this backwards is a 180 degree error that looks exactly like a
    correct capture of an upside-down game, so it is worth spelling out.
    """
    src = pixels.reshape(h, w)
    if rot == 1:
        src = _np.fliplr(src.T)
    elif rot == 2:
        src = src[::-1, ::-1]
    elif rot == 3:
        src = _np.flipud(src.T)

    # Panel byte order -> native, the same swap the scalar path does per pixel.
    v = ((src & 0x00FF) << 8) | (src >> 8)

    out = _np.empty(v.shape + (3,), dtype=_np.uint8)
    out[..., 0] = _R5_LUT[(v >> 11) & 0x1F]
    out[..., 1] = _G6_LUT[(v >> 5) & 0x3F]
    out[..., 2] = _R5_LUT[v & 0x1F]
    return out.tobytes()


class FrameWriter:
    """Writes frames as they arrive instead of collecting them first.

    Buffering was fine for a ten second clip and impossible for a playthrough.
    A frame is 480*480*3 = 691,200 bytes of RGB888, so even at the five frames
    a second the link manages, ten minutes of capture is well over a gigabyte
    of RAM and an hour is out of the question entirely.

    Streaming makes the length of a recording a question about disk rather
    than about memory, and it means an interruption costs the tail of the video
    instead of all of it.
    """

    def __init__(self, out_dir, fps=FPS, w=WIDTH, h=HEIGHT, fragmented=False):
        """fps=None means measure it.

        Live capture has no nominal rate -- it is whatever the link and the game
        managed between them -- and ffmpeg needs a rate before the first frame.
        So the first few frames are held back, timed, and the encoder is started
        with the real figure. Sixteen frames is about three seconds of wall
        clock and eleven megabytes, which buys a rate accurate enough that a
        recording lasting an hour still ends when it should.
        """
        import shutil
        import subprocess
        import time

        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.n = 0
        self._w, self._h = w, h
        self._proc = None
        self._dir = None
        self._out_dir = out_dir
        self._fragmented = fragmented
        self._fps = fps
        self._pending = []          # frames held while the rate is measured
        self._t0 = None

        if fps is None:
            self.path = None        # decided when the encoder is started
            return

        self._start(fps, stamp)

    @property
    def fps(self):
        """The rate the video will actually play at.

        Nominal in fixed mode. In live mode it is the measured arrival rate, or
        a running estimate from the frames held so far -- callers want to report
        seconds-of-video from the first frame, well before enough have arrived
        to settle on a figure.
        """
        import time

        if self._fps is not None:
            return self._fps
        if self._t0 is None or len(self._pending) < 2:
            return FPS
        return max(1.0, (len(self._pending) - 1) / max(0.001, time.time() - self._t0))

    def _start(self, fps, stamp=None):
        import shutil
        import subprocess
        import time

        stamp = stamp or time.strftime("%Y%m%d-%H%M%S")
        out_dir, w, h, fragmented = self._out_dir, self._w, self._h, self._fragmented

        if shutil.which("ffmpeg"):
            self.path = os.path.join(out_dir, f"phantom-{stamp}.mp4")
            args = ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                    "-s", f"{w}x{h}", "-r", str(fps), "-i", "-",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16"]
            if fragmented:
                # An unbounded recording may be ended by something other than a
                # clean stop -- a closed lid, an unplugged board. A fragmented
                # mp4 stays playable when truncated, where a normal one needs
                # its trailer written and is worthless without it.
                args += ["-movflags", "frag_keyframe+empty_moov"]
            args += [self.path]
            self._proc = subprocess.Popen(
                args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        else:
            self._dir = os.path.join(out_dir, f"phantom-{stamp}")
            os.makedirs(self._dir, exist_ok=True)
            self.path = self._dir

    def write(self, rgb):
        import time

        # Measuring phase: hold the frame, and once there are enough of them
        # work out the rate they actually arrived at and open the encoder.
        if self._fps is None:
            now = time.time()
            if self._t0 is None:
                self._t0 = now
            self._pending.append(rgb)
            # Counted by length while buffering, then reset so the flush can
            # count them exactly once. Incrementing in both places double-counts
            # every held frame, which shows up as a progress bar that overshoots
            # and a frame total that disagrees with the file.
            self.n = len(self._pending)
            if len(self._pending) >= 16:
                span = max(0.001, now - self._t0)
                self._fps = max(1.0, min(60.0, (len(self._pending) - 1) / span))
                self._start(self._fps)
                held, self._pending = self._pending, []
                self.n = 0
                for f in held:
                    self._flush_one(f)
            return

        self._flush_one(rgb)

    def _flush_one(self, rgb):
        if self._proc:
            self._proc.stdin.write(rgb)
        else:
            with open(os.path.join(self._dir, f"f{self.n:05d}.ppm"), "wb") as fh:
                fh.write(b"P6\n%d %d\n255\n" % (self._w, self._h))
                fh.write(rgb)
        self.n += 1

    def close(self):
        # A recording stopped before the rate settled still has to be written,
        # and at the best rate available rather than the nominal one -- a live
        # clip of ten frames written at 30fps plays in a third of a second.
        # self.fps is the running estimate, which two frames is enough for.
        if self._fps is None:
            self._fps = self.fps
            self._start(self._fps)
            held, self._pending = self._pending, []
            self.n = 0
            for f in held:
                self._flush_one(f)
        if self._proc:
            try:
                self._proc.stdin.close()
                self._proc.wait(timeout=60)
            except Exception:
                self._proc.kill()
            self._proc = None
        return self.path


def subsample_ppm(rgb, w, h, out_w):
    """Nearest-neighbour down to a small PPM, for on-screen preview. PPM because
    Tk reads it natively -- no image library, nothing extra to bundle."""
    step = w // out_w
    out_h = h // step

    if _np is not None:
        a = _np.frombuffer(rgb, dtype=_np.uint8).reshape(h, w, 3)
        small = a[::step, ::step][:out_h, :out_w]
        return b"P6\n%d %d\n255\n" % (out_w, out_h) + small.tobytes()

    body = bytearray(out_w * out_h * 3)
    k = 0
    for yy in range(out_h):
        row = (yy * step) * w
        for xx in range(out_w):
            o = (row + xx * step) * 3
            body[k:k + 3] = rgb[o:o + 3]
            k += 3
    return b"P6\n%d %d\n255\n" % (out_w, out_h) + bytes(body)
