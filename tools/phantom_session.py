#!/usr/bin/env python3
"""
Record a Phantom session, then render it to video at a true 60 fps.

    python tools/phantom_session.py record --port COM6 --out run.phr
    python tools/phantom_session.py render --port COM6 run.phr --dir .

Why two steps. A frame is 460,800 bytes and the link carries 0.74 MB/s, so
pixels cannot come off the device at 60 fps -- about 23 is the ceiling. But the
SIMULATION is a dt and an input struct, under a hundred bytes a frame. Recording
logs that, which is free, and the game runs at its true unimpeded speed while
you play. Rendering then re-runs the session on the device frame by frame and
pulls the pixels at whatever rate the link manages.

The video is a real 60 fps because the frames really were 1/60s apart when they
happened. Rendering takes a few minutes per minute of gameplay; it is a wait,
not a compromise -- every frame is the genuine rasteriser output, and the HUD's
own fps counter reads whatever it read at the time.

Recording RESTARTS the game, because a session has to begin somewhere the replay
can also begin. Play from the menu.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import (Desync, FrameWriter, PhantomLink, Session,
                          reset_board)


def record(args):
    print("resetting the board...")
    reset_board(args.port)
    link = PhantomLink(args.port)
    link.open()
    hdr = link.session_start()
    ses = Session(hdr)
    print(f"recording -- play now, Ctrl+C to stop  "
          f"(input struct {hdr['blob']}B, {len(hdr['seeds'])//4} seeds)")
    started = time.time()
    try:
        while True:
            ses.frames.append(link.session_frame())
            if len(ses.frames) % 30 == 0:
                print(f"\r  {len(ses.frames)} frames  {ses.seconds:6.1f}s of play"
                      f"  {len(ses.frames)/max(0.001, time.time()-started):5.1f} fps",
                      end="", flush=True)
    except KeyboardInterrupt:
        print("\nstopped")
    except (Desync, TimeoutError) as e:
        print(f"\nlink problem: {e}")
    finally:
        try:
            link.session_end()
        except Exception:
            pass
        link.close()

    if not ses.frames:
        sys.exit("no frames recorded -- is this the port marked [ESP32]?")
    ses.save(args.out)
    fps = len(ses.frames) / max(0.001, ses.seconds)
    print(f"wrote {args.out}  ({len(ses.frames)} frames, {ses.seconds:.1f}s "
          f"of gameplay at {fps:.1f} fps, {os.path.getsize(args.out)/1024:.0f} KB)")


def render(args):
    ses = Session.load(args.session)
    n = len(ses.frames)
    fps = n / max(0.001, ses.seconds)
    print(f"{n} frames, {ses.seconds:.1f}s of gameplay at {fps:.1f} fps")

    print("resetting the board...")
    reset_board(args.port)
    link = PhantomLink(args.port)
    link.open()
    link.replay_start(ses.hdr)   # blocks until the device reports PLAYING

    # Encoded at the rate the session was actually played, so the video runs at
    # the speed the game ran -- not at the speed the pixels crawled off it.
    writer = FrameWriter(args.dir, fps=round(fps, 3), fragmented=True)
    started = time.time()
    done = 0

    # One record always queued ahead. Strict lockstep leaves the device idle
    # between frames waiting to be asked for the next one, and its final bytes
    # do not leave the USB driver until something else moves them -- the host
    # then waits eight seconds for a hundred bytes that only arrive once it
    # gives up and writes. Keeping a request in flight means the device is never
    # idle, and it renders the next frame while the host encodes this one.
    # Two is enough. Six measured identically (19.5 fps both), which says the
    # remaining cost is the link and the device's own render, not idle time.
    depth = 2
    for fr in ses.frames[:depth]:
        link.replay_send(fr)

    try:
        for i, fr in enumerate(ses.frames):
            nxt = i + depth
            if nxt < len(ses.frames):
                link.replay_send(ses.frames[nxt])
            try:
                rgb, _w, _h = link.read_frame()
            except Desync as e:
                print(f"\n  {e}, resyncing...")
                continue
            writer.write(rgb)
            done += 1
            el = time.time() - started
            rate = done / max(0.001, el)
            print(f"\r  {done}/{n}  {rate:5.1f} fps  "
                  f"eta {(n-done)/max(0.01, rate):5.0f}s", end="", flush=True)
    except KeyboardInterrupt:
        print("\nstopped -- keeping what rendered")
    except TimeoutError as e:
        print(f"\nstopped early: {e}")
    finally:
        try:
            link.session_end()
        except Exception:
            pass
        link.close()
        path = writer.close()

    el = time.time() - started
    print(f"\nwrote {path}  ({done} frames at {fps:.1f} fps = "
          f"{done/max(1.0,fps):.1f}s of video, {el:.0f}s elapsed, "
          f"{el/max(0.001, done/max(1.0,fps)):.1f}x realtime)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record", help="log a session while you play at full speed")
    r.add_argument("--port", required=True)
    r.add_argument("--out", default="session.phr")
    r.set_defaults(fn=record)

    p = sub.add_parser("render", help="re-run a session and pull the pixels")
    p.add_argument("session")
    p.add_argument("--port", required=True)
    p.add_argument("--dir", default=".")
    p.set_defaults(fn=render)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
