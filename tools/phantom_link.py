"""
Device link for Phantom frame capture.

The wire protocol and the pixel conversions live here rather than in either
front end, so the CLI and the recorder cannot drift apart. Both bugs found in
the first version -- scanning for the frame magic inside binary payloads, and
copying the firmware's rotation instead of inverting it -- were the kind that
get fixed in one copy and left in the other.
"""

import struct
import time

import serial
import serial.tools.list_ports

FPS = 30            # the rate the firmware steps at while armed
WIDTH = HEIGHT = 480


def list_ports():
    """Likely boards first: the ESP32-S3 shows up as a USB CDC device."""
    ports = list(serial.tools.list_ports.comports())
    def score(p):
        blob = f"{p.description} {p.manufacturer or ''} {p.hwid or ''}".lower()
        return 0 if any(k in blob for k in ("esp32", "usb serial", "cdc", "espressif")) else 1
    ports.sort(key=score)
    return [(p.device, p.description or p.device) for p in ports]


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

    def arm(self):
        self._synced = False
        self.ser.write(b"c")
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
