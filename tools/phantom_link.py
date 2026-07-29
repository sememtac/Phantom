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
import time

import serial
import serial.tools.list_ports

FPS = 30            # the rate the firmware steps at while armed
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


class PhantomLink:
    def __init__(self, port, baud=115200, timeout=8):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self._synced = False

    # -- connection ---------------------------------------------------------

    def open(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
        time.sleep(0.4)
        self.ser.reset_input_buffer()

    def close(self):
        if self.ser:
            try:
                self.ser.write(b"s")
                self.ser.flush()
            except Exception:
                pass
            self.ser.close()
            self.ser = None

    def arm(self, live=False):
        """Fixed-step gives smooth video of a slowed game; live gives real-time
        video of the game as it actually ran."""
        self._synced = False
        self.ser.write(b"l" if live else b"c")
        self.ser.flush()

    def disarm(self):
        self.ser.write(b"s")
        self.ser.flush()

    # -- stream -------------------------------------------------------------

    def _read_exact(self, n):
        """Serial reads come back short constantly and a frame is fifteen bands;
        every one of them will be split. Looping here rather than at each call
        site is the difference between working and appearing to work."""
        buf = bytearray()
        while len(buf) < n:
            chunk = self.ser.read(n - len(buf))
            if not chunk:
                raise TimeoutError(f"link went quiet with {n - len(buf)} bytes outstanding")
            buf += chunk
        return bytes(buf)

    def _scan_to(self, magic):
        window = bytearray()
        while True:
            b = self.ser.read(1)
            if not b:
                raise TimeoutError("no frame header; is the device armed?")
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

        pixels = [0] * (w * h)
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


def to_rgb(pixels, w, h, rot):
    """RGB565 in PANEL byte order and PANEL orientation to upright RGB888.

    Both conversions have to happen somewhere. The device stores pixels
    pre-swapped so a band blit is a straight DMA, and never un-rotates because
    the panel is mounted a quarter turn off -- doing either on the board would
    cost frame time purely to save the host a loop.
    """
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
    body = bytearray(out_w * out_h * 3)
    k = 0
    for yy in range(out_h):
        row = (yy * step) * w
        for xx in range(out_w):
            o = (row + xx * step) * 3
            body[k:k + 3] = rgb[o:o + 3]
            k += 3
    return b"P6\n%d %d\n255\n" % (out_w, out_h) + bytes(body)
