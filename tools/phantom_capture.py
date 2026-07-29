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
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import FPS, Desync, FrameWriter, PhantomLink


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. COM6 or /dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="length of the resulting VIDEO, not of the wait")
    ap.add_argument("--continuous", action="store_true",
                    help="record until Ctrl+C -- for a whole playthrough")
    ap.add_argument("--dir", default=".", help="output folder")
    args = ap.parse_args()

    want = None if args.continuous else max(1, int(args.seconds * FPS))
    link = PhantomLink(args.port)
    writer = None
    started = time.time()

    if want:
        print(f"arming for {want} frames ({args.seconds:.1f}s of video)...")
    else:
        print("arming -- recording until Ctrl+C...")

    try:
        # Frames are written as they arrive rather than collected. At 691,200
        # bytes each, an unbounded recording fills memory long before it fills
        # a disk.
        writer = FrameWriter(args.dir, fragmented=args.continuous)
        link.open()
        link.arm()
        while want is None or writer.n < want:
            try:
                rgb, _w, _h = link.read_frame()
            except Desync as e:
                print(f"\n  {e}, resyncing...")
                continue
            writer.write(rgb)
            rate = writer.n / max(0.001, time.time() - started)
            if want:
                eta = (want - writer.n) / max(0.01, rate)
                print(f"\r  {writer.n}/{want}  {rate:.1f} fps  eta {eta:5.0f}s",
                      end="", flush=True)
            else:
                print(f"\r  {writer.n} frames  {writer.n / FPS:6.1f}s of video"
                      f"  {rate:.1f} fps", end="", flush=True)
    except KeyboardInterrupt:
        print("\nstopped -- keeping what arrived")
    except TimeoutError as e:
        print(f"\nstopped early: {e}")
    finally:
        link.close()
        path = writer.close() if writer else None
        n = writer.n if writer else 0

    if not n:
        sys.exit("\nno frames captured -- is the board running?")
    print(f"\nwrote {path}  ({n} frames, {n / FPS:.1f}s of video, "
          f"{time.time() - started:.0f}s elapsed)")


if __name__ == "__main__":
    main()
