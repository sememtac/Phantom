#!/usr/bin/env python3
"""
Record Phantom off the device over USB serial, from the command line.

    python tools/phantom_capture.py --port COM6 --seconds 12 --out demo.mp4

For a window with a folder picker and a live preview, use phantom_recorder.py
(or the built PhantomRecorder.exe) instead. Both drive the same link module, so
neither can drift away from the protocol the firmware actually speaks.

The device does not stream in real time and does not try to. A frame is 460,800
bytes and the CDC link carries roughly a megabyte a second, so while recording
is armed the firmware steps its simulation at a fixed 30 fps regardless of how
long each frame actually took. The board runs in slow motion; the recording
plays back smooth. Wall-clock speed only decides how long you wait.

So --seconds is the length of the VIDEO, not the length of the wait. Twelve
seconds of footage is 360 frames and takes a few minutes to pull.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import FPS, HEIGHT, WIDTH, Desync, PhantomLink


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. COM6 or /dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="length of the resulting VIDEO, not of the wait")
    ap.add_argument("--out", default="phantom.mp4")
    args = ap.parse_args()

    want = max(1, int(args.seconds * FPS))
    link = PhantomLink(args.port)
    frames = []
    started = time.time()

    print(f"arming for {want} frames ({args.seconds:.1f}s of video)...")
    try:
        link.open()
        link.arm()
        while len(frames) < want:
            try:
                rgb, _w, _h = link.read_frame()
            except Desync as e:
                print(f"\n  {e}, resyncing...")
                continue
            frames.append(rgb)
            rate = len(frames) / max(0.001, time.time() - started)
            eta = (want - len(frames)) / max(0.01, rate)
            print(f"\r  {len(frames)}/{want}  {rate:.1f} fps captured  eta {eta:5.0f}s",
                  end="", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted -- keeping what arrived")
    except TimeoutError as e:
        print(f"\nstopped early: {e}")
    finally:
        link.close()

    if not frames:
        sys.exit("\nno frames captured -- is the board running?")
    print(f"\n{len(frames)} frames in {time.time() - started:.0f}s")

    if shutil.which("ffmpeg"):
        print(f"encoding {args.out} ...")
        p = subprocess.Popen(
            ["ffmpeg", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", f"{WIDTH}x{HEIGHT}", "-r", str(FPS), "-i", "-",
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
                fh.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
                fh.write(f)
        print(f"ffmpeg not found; wrote {len(frames)} PPMs to {d}/")
        print(f"  ffmpeg -r {FPS} -i {d}/f%05d.ppm -c:v libx264 -pix_fmt yuv420p {args.out}")


if __name__ == "__main__":
    main()
