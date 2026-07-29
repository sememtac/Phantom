"""
The link to the device, for Phantom frame capture.

This module holds the wire protocol and the pixel conversions. It does not hold
them in the command line tool or in the window, so those two programs cannot
become different. The first version had two bugs of that type: it looked for the
frame magic inside binary data, and it copied the rotation of the firmware
instead of the inverse. A person fixes such a bug in one copy and leaves it in
the other copy.
"""

import os
import struct
import threading
import time

import serial
import serial.tools.list_ports

# numpy makes the pixel work for each frame very small. A Python loop that
# decodes the runs and rotates 230,400 pixels costs about 150 ms for each frame.
# That limited the capture to 5 fps while the link delivered 22 fps. The numpy
# code needs a few milliseconds.
#
# numpy is optional. The Python code below is kept and is still correct, so a
# computer without numpy records slowly instead of not at all.
try:
    import numpy as _np
except ImportError:
    _np = None

FPS = 60            # what the game targets, and what a session records at
WIDTH = HEIGHT = 480


ESPRESSIF_VID = 0x303A


def list_ports():
    """List the ports, with the device first. Sort by the USB VENDOR ID.

    A sort by the description text looks correct and selects the wrong port. An
    FTDI adapter has the name "USB Serial Port (COM3)". The device has the name
    "USB Serial Device (COM6)". Both names contain "usb serial", so both get the
    same score, and the lower port number comes first. The window then opened a
    port with no device on it. It waited for a header that never came, and it
    told the user that the device did not run.

    The vendor ID is not a guess. 0x303A belongs to Espressif, and no other
    equipment on a normal computer uses it.
    """
    ports = list(serial.tools.list_ports.comports())

    def score(p):
        if p.vid == ESPRESSIF_VID:
            return 0
        blob = f"{p.description} {p.manufacturer or ''}".lower()
        if "esp32" in blob or "espressif" in blob:
            return 1
        return 2 if p.vid else 3          # a port with a VID is better than Bluetooth

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
    """Pulse the reset line of the device and wait for the device to start.

    A render is an exchange of one entry for one frame. If the host stops during
    a render, the device waits for the next entry and reads every byte as one.
    The device recovers after 30 seconds, and during that time it looks broken.

    A start from a known state is simpler than a test for each way the last run
    could end. The record step and the render step both start the game again, so
    the reset costs nothing.
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
        self._blob = 0          # size of the input structure, from the header

    # -- connection ---------------------------------------------------------

    def open(self):
        # Set DTR and RTS to false BEFORE the port opens. If you do not, the
        # open resets the device. During a render this is not safe. The 'P'
        # command arrives while the device starts, so the device loses it. The
        # device then reads the entries that follow as commands. One of those
        # bytes is another 'P', which starts a render from the next bytes.
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baud
        self.ser.timeout = self.timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()

        # Windows gives a serial port a receive buffer of about 4 KB. A frame is
        # 35 KB and arrives in one burst. The host does work between the bands:
        # it decodes the runs and writes them into the frame. The buffer of the
        # driver therefore overflows and DISCARDS bytes. There is no error. The
        # result is a frame that stops in the middle of a band. The device sent
        # every byte and then waited for the next request.
        try:
            self.ser.set_buffer_size(rx_size=1 << 20, tx_size=1 << 16)
        except Exception:
            pass          # Windows only. Other systems have a large default.

        time.sleep(0.4)
        self.ser.reset_input_buffer()
        self._start_reader()

    # -- receive thread -----------------------------------------------------
    #
    # One thread reads the port and a different thread parses the data. One
    # thread for both loses data. A frame arrives as a burst of 35 KB while the
    # parser decodes the runs and fills a numpy array. Each millisecond of that
    # work is a millisecond in which nothing empties the receive buffer of the
    # driver. The buffer overflows and discards bytes, with no error.
    #
    # A measurement: 3.6 KB were lost from 1.6 MB, and the device reported that
    # it wrote every byte with no short write and no stop.
    #
    # This thread therefore only moves bytes out of the driver. The parser then
    # reads a buffer in memory, where slow work costs time instead of data.

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
    # The host does not read the contents of a session entry. It saves the input
    # structure of the device and sends it back byte for byte. A new field in
    # VgInput therefore does not need a change in this file. The device compares
    # the size at the start of a render and refuses a session that does not
    # match. The host cannot make that test.

    def session_start(self):
        """Start a session. Returns the header to save with the session."""
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
        """Read one entry of the session."""
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

        # Wait for the report from the device. Do not wait for a fixed time.
        # The device starts the game again first. This makes a new sky and takes
        # some hundred milliseconds. An entry that arrives during that time
        # becomes a command.
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
                    "The device refused the session. A different firmware "
                    "build made it.")
        raise TimeoutError("device never started the replay")

    def replay_send(self, fr):
        self.ser.write(b"PHRP")
        self.ser.write(struct.pack("<fB", fr["dt"], len(fr["seeds"]) // 4))
        self.ser.write(fr["seeds"])
        self.ser.write(fr["input"])
        self.ser.flush()

    # -- stream -------------------------------------------------------------

    def _read_exact(self, n):
        """Read exactly n bytes.

        A serial read gives fewer bytes than you ask for. A frame has 15 bands
        and the driver divides all of them. The loop is here, and not at each
        call, so that every caller is correct."""
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
        """Read one frame as upright RGB888 bytes.

        Raises Desync if the stream breaks inside a frame. The caller can then
        ask for the next frame."""
        # Synchronise ONCE, then read each tag at its position.
        #
        # A search before every frame looks safer and is not. The data of a band
        # is binary, so the four bytes 'PHFR' also occur inside the compressed
        # pixels. The search then stops at the middle of a band and reads it as
        # a header.
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


# Tables from 5/6/5 to 8/8/8, built one time. A shift for each pixel in Python
# is the slowest operation in this path, and a frame has 230,400 pixels.
_R5 = [(v * 255) // 31 for v in range(32)]
_G6 = [(v * 255) // 63 for v in range(64)]

if _np is not None:
    _R5_LUT = _np.array(_R5, dtype=_np.uint8)
    _G6_LUT = _np.array(_G6, dtype=_np.uint8)


_GAMMA = 1.0
_SCALE = None          # per-brightness gain, indexed by the pixel's max channel


def set_gamma(g):
    """Make the picture brighter. The hue and the saturation do not change.

    A capture holds the same values as the framebuffer: 0x1F becomes 255. It
    cannot hold the PANEL. The panel is an emissive AMOLED with true black, and
    it runs at high brightness, so the same values look brighter there than on a
    monitor. This function therefore matches the panel. It does not correct the
    capture. A value of 1.0 keeps the framebuffer values and is the default.

    The function does NOT apply a curve to each channel. That is the usual
    method and it is wrong here. The amber of the HUD is #ffae18: red is 255,
    green is in the middle, and blue is very low. A curve on each channel cannot
    raise the red, so only green and blue rise. The colour then turns towards
    yellow and loses saturation. Measurements on that colour: 39 degrees and 91%
    saturation at gamma 1.0, and 43 degrees and 79% at gamma 1.5. The interface
    uses this one amber colour almost everywhere, so a user sees that change
    first.

    The function applies the gain to the VALUE of the pixel, which is its
    largest channel. It then multiplies all three channels by that same gain.
    The ratios between the channels do not change, so the hue and the saturation
    do not change. Only the brightness changes.

    A pixel with a channel at 255 cannot become brighter. This is correct,
    because 255 is the maximum of the format. The gain applies to all values
    below it.
    """
    global _GAMMA, _SCALE
    _GAMMA = max(0.01, float(g))
    if _GAMMA == 1.0 or _np is None:
        _SCALE = None
        return
    inv = 1.0 / _GAMMA
    v = _np.arange(256, dtype=_np.float32)
    out = _np.power(v / 255.0, inv) * 255.0
    with _np.errstate(divide="ignore", invalid="ignore"):
        _SCALE = _np.where(v > 0, out / _np.maximum(v, 1e-6), 1.0).astype(_np.float32)


def _lift(out):
    """Multiply all three channels by the gain of the largest channel."""
    if _SCALE is None:
        return out
    mx = out.max(axis=-1)
    return _np.clip(out * _SCALE[mx][..., None] + 0.5, 0, 255).astype(_np.uint8)


def to_rgb(pixels, w, h, rot):
    """Convert RGB565 to upright RGB888.

    The input has the byte order of the panel and the orientation of the panel.
    Some program must do both conversions. The device stores each pixel with the
    bytes in the order of the panel, so a band goes to the display by DMA. The
    device also does not correct the rotation, because the panel is mounted at
    90 degrees. Both operations on the device would cost frame time and would
    only save the host a loop.
    """
    if _np is not None and isinstance(pixels, _np.ndarray):
        return _to_rgb_np(pixels, w, h, rot)

    rgb = bytearray(w * h * 3)

    # This is the INVERSE of rot_pt in the firmware. It is not a copy. A copy
    # gives an error of 180 degrees for rot 1 and rot 3. That error looks the
    # same as a correct capture of a game that is upside down.
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
    """A session: one header and one entry for each frame.

    A session stays in memory, and you can keep it. A session of ten minutes at
    60 fps has 36,000 entries of less than 100 bytes, which is about three
    megabytes. This is the method of these tools: the pixels are not the data
    that is necessary to keep.
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
    """The numpy form of the loop in to_rgb. It uses the same inverse rotation.

    The rotation below is derived, not copied. For rot 1 the firmware maps the
    logical point (lx,ly) to the panel point (ly, H-1-lx). The panel array S
    therefore inverts to D[x, h-1-y] = S[y, x]. That is S transposed, with the
    columns in the opposite order. For rot 3 the transpose is flipped on the
    other axis. For rot 2 both axes are in the opposite order.

    An error here of 180 degrees looks the same as a correct capture of a game
    that is upside down, so the derivation is written out.
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
    return _lift(out).tobytes()


class FrameWriter:
    """Write each frame when it arrives. Do not collect the frames first.

    A buffer is acceptable for a clip of ten seconds and is not possible for a
    full game. A frame is 480 x 480 x 3 = 691,200 bytes of RGB888. At five
    frames each second, ten minutes of capture is more than one gigabyte of RAM,
    and one hour is not possible.

    A write for each frame makes the length of a video a question of disk space,
    not of memory. An interruption then costs the end of the video, not all of
    the video.
    """

    def __init__(self, out_dir, fps=FPS, w=WIDTH, h=HEIGHT, fragmented=False):
        """An fps of None tells the writer to measure the rate.

        ffmpeg needs a rate before the first frame. If the caller does not know
        the rate, the writer keeps the first frames, measures the time between
        them, and then starts ffmpeg with the measured rate. Sixteen frames is
        about three seconds and eleven megabytes. That number of frames gives a
        rate that is correct enough for a video of one hour.
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
        """The rate at which the video plays.

        This is the rate the caller gave, or the measured rate. During the
        measurement it is an estimate from the frames that arrived. A caller
        shows the length of the video from the first frame, and that is before
        the measurement is complete.
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
            # Declare the colour. Without a declaration the file says nothing
            # about its YUV range, so each player must guess. A player that
            # guesses full range on a file with limited range makes the black
            # areas darker. This game is almost all black, so a wrong guess is
            # very visible.
            args = ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                    "-s", f"{w}x{h}", "-r", str(fps), "-i", "-",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16",
                    "-colorspace", "bt709", "-color_primaries", "bt709",
                    "-color_trc", "bt709", "-color_range", "tv",
                    # Give the same values to the encoder. The -color_* options
                    # of ffmpeg write the tags of the container. For two of the
                    # three values they do not write the VUI of the video
                    # stream. A player that reads the stream must then guess.
                    "-x264-params",
                    "colorprim=bt709:transfer=bt709:colormatrix=bt709"]
            if fragmented:
                # A long video can stop for a reason other than a clean stop:
                # a closed lid, or a disconnected device. A fragmented mp4 file
                # still plays if it is incomplete. A normal mp4 file needs its
                # trailer, and without the trailer it has no value.
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

        # Measurement: keep the frame. When there are enough frames, calculate
        # the rate at which they arrived and start the encoder.
        if self._fps is None:
            now = time.time()
            if self._t0 is None:
                self._t0 = now
            self._pending.append(rgb)
            # During the measurement the count is the length of the list. The
            # count is then set to zero, so that the write counts each frame one
            # time. A count in both places counts each kept frame two times.
            # The progress bar then goes too far, and the total does not agree
            # with the file.
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
        # A video that stops before the measurement is complete must still be
        # written, and at the best known rate. Ten frames at 60 fps play in one
        # sixth of a second, which is wrong. self.fps holds the estimate, and
        # two frames are enough for it.
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
    """Make a small PPM image for the window. Use nearest-neighbour sampling.

    The format is PPM because Tk reads PPM without help. No image library is
    necessary, and the program does not become larger."""
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
