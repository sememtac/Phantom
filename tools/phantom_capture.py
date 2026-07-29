#!/usr/bin/env python3
"""
Record Phantom off the device over USB serial.

    python tools/phantom_capture.py --port COM6 --seconds 12 --out demo.mp4

The device does not stream in real time and does not try to. A frame is 460,800
bytes and the CDC link carries roughly a megabyte a second, so while recording
is armed the firmware steps its simulation at a fixed 30 fps regardless of how
long each frame actually took. The board runs in slow motion; the recording
plays back smooth. Wall-clock speed only decides how long you wait.

That means --seconds is the length of the VIDEO, not the length of the wait.
Twelve seconds of footage is 360 frames and will take a few minutes to pull.

Needs pyserial. Writes an mp4 if ffmpeg is on PATH, otherwise a directory of
PPMs, which ffmpeg or almost anything else can turn into a video later.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

FPS = 30

def read_exact(ser, n):
    """Serial reads come back short constantly; a frame is 15 bands and every
    one of them will be split. Looping here rather than at each call site is
    the difference between this working and appearing to work."""
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError(f"link went quiet with {n - len(buf)} bytes outstanding")
        buf += chunk
    return bytes(buf)


def sync_to(ser, magic):
    """Slide along the stream until the magic lands.

    Necessary because the firmware prints human-readable lines either side of a
    capture, and because a run may be started while the port already has
    telemetry queued. Costs nothing once locked -- the very next read matches."""
    window = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("no frame header; is the device armed?")
        window += b
        if len(window) > 4:
            del window[0]
        if bytes(window) == magic:
            return


def decode_rle(payload, npix):
    """u8 run, u16 pixel. Returns a list of RGB565 values in panel byte order."""
    out = []
    i = 0
    n = len(payload)
    while i + 2 < n:
        run = payload[i]
        val = payload[i + 1] | (payload[i + 2] << 8)
        out.extend([val] * run)
        i += 3
    if len(out) != npix:
        out = (out + [0] * npix)[:npix]
    return out


def to_rgb(pixels, w, h, rot):
    """RGB565 in PANEL byte order and PANEL orientation to RGB888 upright.

    Two conversions, both of which have to happen somewhere. The device stores
    pixels pre-swapped so a band blit is a straight DMA, and it never un-rotates
    because the panel is mounted a quarter turn off -- doing either on the board
    would cost frame time purely to save the host a loop, so the host does it."""
    rgb = bytearray(w * h * 3)
    for i, v in enumerate(pixels):
        v = ((v & 0xFF) << 8) | (v >> 8)          # panel order -> native
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31

        # (x, y) here are PANEL coordinates, and what is wanted is the LOGICAL
        # position they came from -- so this is the INVERSE of the firmware's
        # rot_pt, not a copy of it. Getting that backwards is a 180 degree error
        # for rot 1 and 3 and looks exactly like a correct capture of an
        # upside-down game, which is how it survived the first test.
        #
        #   rot 1: firmware maps logical (lx,ly) -> panel (ly, H-1-lx)
        #          so logical = (H-1-py, px)
        #   rot 3: firmware maps logical (lx,ly) -> panel (W-1-ly, lx)
        #          so logical = (py, W-1-px)
        x, y = i % w, i // w
        if rot == 1:      sx, sy = (h - 1 - y), x
        elif rot == 2:    sx, sy = (w - 1 - x), (h - 1 - y)
        elif rot == 3:    sx, sy = y, (w - 1 - x)
        else:             sx, sy = x, y

        o = (sy * w + sx) * 3
        rgb[o:o + 3] = bytes((r, g, b))
    return bytes(rgb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. COM6 or /dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="length of the resulting VIDEO, not of the wait")
    ap.add_argument("--out", default="phantom.mp4")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    want = max(1, int(args.seconds * FPS))

    ser = serial.Serial(args.port, args.baud, timeout=8)
    time.sleep(0.4)
    ser.reset_input_buffer()

    print(f"arming for {want} frames ({args.seconds:.1f}s of video)...")
    ser.write(b"c")
    ser.flush()

    frames = []
    started = time.time()
    synced = False
    try:
        while len(frames) < want:
            # Sync ONCE, then read tags positionally.
            #
            # Scanning for the magic before every frame looks more robust and is
            # the opposite: band payloads are arbitrary binary, so the four bytes
            # 'PHFR' turn up inside compressed pixel data soon enough, and the
            # scanner locks onto one and treats the middle of a band as a frame
            # header. That is what desynchronised the first version after
            # exactly one good frame. Once the stream is located, the next four
            # bytes after a frame ends ARE the next header -- so read them, and
            # only fall back to scanning if they are wrong.
            if not synced:
                sync_to(ser, b"PHFR")
                synced = True
            else:
                tag = read_exact(ser, 4)
                if tag != b"PHFR":
                    print("\n  resyncing...")
                    sync_to(ser, b"PHFR")

            idx, w, h, rot, fmt = struct.unpack("<IHHBB", read_exact(ser, 10))
            if fmt != 1:
                sys.exit(f"unknown pixel format {fmt}")

            pixels = [0] * (w * h)
            while True:
                tag = read_exact(ser, 4)
                if tag == b"PHEN":
                    read_exact(ser, 4)
                    break
                if tag != b"PHBD":
                    # Lost the thread mid-frame. Drop this frame and pick the
                    # stream back up rather than writing garbage into the video.
                    print("\n  lost a frame, resyncing...")
                    synced = False
                    pixels = None
                    break
                y, bh, nbytes = struct.unpack("<HHI", read_exact(ser, 8))
                band = decode_rle(read_exact(ser, nbytes), w * bh)
                pixels[y * w:(y + bh) * w] = band

            if pixels is None:
                continue
            frames.append(to_rgb(pixels, w, h, rot))

            done = len(frames)
            rate = done / max(0.001, time.time() - started)
            eta  = (want - done) / max(0.01, rate)
            print(f"\r  {done}/{want}  {rate:.1f} fps captured  eta {eta:5.0f}s",
                  end="", flush=True)
    except (TimeoutError, ValueError) as e:
        print(f"\nstopped early: {e}")
    finally:
        ser.write(b"s")
        ser.flush()
        ser.close()

    if not frames:
        sys.exit("\nno frames captured")

    print(f"\n{len(frames)} frames in {time.time() - started:.0f}s")

    w = h = 480
    if shutil.which("ffmpeg"):
        print(f"encoding {args.out} ...")
        p = subprocess.Popen(
            ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", f"{w}x{h}", "-r", str(FPS), "-i", "-",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16", args.out],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for f in frames:
            p.stdin.write(f)
        p.stdin.close()
        p.wait()
        print(f"wrote {args.out}")
    else:
        d = os.path.splitext(args.out)[0] + "_frames"
        os.makedirs(d, exist_ok=True)
        for i, f in enumerate(frames):
            with open(os.path.join(d, f"f{i:05d}.ppm"), "wb") as fh:
                fh.write(b"P6\n%d %d\n255\n" % (w, h))
                fh.write(f)
        print(f"ffmpeg not found; wrote {len(frames)} PPMs to {d}/")
        print(f"  ffmpeg -r {FPS} -i {d}/f%05d.ppm -c:v libx264 -pix_fmt yuv420p {args.out}")


if __name__ == "__main__":
    main()
