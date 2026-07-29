#!/usr/bin/env python3
"""
Record Phantom off the device over USB serial, from the command line.

    python tools/phantom_capture.py --port COM6 --seconds 12 --out demo.mp4

For a window with a folder picker and a live preview, use phantom_recorder.py
(or the built PhantomRecorder.exe) instead. Both drive the same link module, so
neither can drift away from the protocol the firmware actually speaks.

A frame is 460,800 bytes and the CDC link carries roughly a megabyte a second,
which is the constraint everything else follows from. Two modes trade against
it in opposite directions:

  --live (default)  the game runs normally and you get whatever the link can
                    carry. Real time, and choppy at a few frames a second.
                    --seconds is wall clock, and is also the video length.

  --smooth          the firmware steps its simulation at a fixed 30 fps however
                    long each frame really took. The board runs in slow motion
                    and the recording plays back perfectly. --seconds is the
                    length of the VIDEO, not of the wait: twelve seconds is 360
                    frames and takes a few minutes to pull.
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
                    help="wall clock in live mode; length of the VIDEO in smooth mode")
    ap.add_argument("--continuous", action="store_true",
                    help="record until Ctrl+C -- for a whole playthrough")
    ap.add_argument("--smooth", action="store_true",
                    help="fixed-step slow motion instead of real time")
    ap.add_argument("--dir", default=".", help="output folder")
    args = ap.parse_args()

    live = not args.smooth
    # Smooth mode counts frames to a known target; live mode counts seconds on
    # the clock, because its frame rate is not known until frames arrive.
    want = want_secs = None
    if not args.continuous:
        if live:
            want_secs = max(0.5, args.seconds)
        else:
            want = max(1, int(args.seconds * FPS))

    link = PhantomLink(args.port)
    writer = None
    started = time.time()

    if want:
        print(f"arming smooth for {want} frames ({args.seconds:.1f}s of video)...")
    elif want_secs:
        print(f"arming live for {want_secs:.1f}s...")
    else:
        print(f"arming {'live' if live else 'smooth'} -- recording until Ctrl+C...")

    try:
        # Frames are written as they arrive rather than collected. At 691,200
        # bytes each, an unbounded recording fills memory long before it fills
        # a disk.
        writer = FrameWriter(args.dir, fps=None if live else FPS,
                             fragmented=args.continuous)
        link.open()
        link.arm(live=live)
        while True:
            elapsed = time.time() - started
            if want is not None and writer.n >= want:
                break
            if want_secs is not None and elapsed >= want_secs:
                break
            try:
                rgb, _w, _h = link.read_frame()
            except Desync as e:
                print(f"\n  {e}, resyncing...")
                continue
            writer.write(rgb)
            elapsed = time.time() - started
            rate = writer.n / max(0.001, elapsed)
            if want:
                eta = (want - writer.n) / max(0.01, rate)
                print(f"\r  {writer.n}/{want}  {rate:.1f} fps  eta {eta:5.0f}s",
                      end="", flush=True)
            elif want_secs:
                print(f"\r  {elapsed:5.1f}/{want_secs:.1f}s  {writer.n} frames"
                      f"  {rate:.1f} fps", end="", flush=True)
            else:
                print(f"\r  {writer.n} frames  "
                      f"{writer.n / max(1.0, writer.fps):6.1f}s of video"
                      f"  {rate:.1f} fps", end="", flush=True)
    except KeyboardInterrupt:
        print("\nstopped -- keeping what arrived")
    except TimeoutError as e:
        print(f"\nstopped early: {e}")
    finally:
        link.close()
        path = writer.close() if writer else None
        n = writer.n if writer else 0
        fps = writer.fps if writer else FPS

    if not n:
        sys.exit("\nno frames captured -- is the board running, and is this the "
                 "port marked [ESP32]?")
    print(f"\nwrote {path}  ({n} frames, {n / max(1.0, fps):.1f}s of video "
          f"@ {fps:.1f}fps, {time.time() - started:.0f}s elapsed)")


if __name__ == "__main__":
    main()
