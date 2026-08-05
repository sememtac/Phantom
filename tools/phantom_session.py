#!/usr/bin/env python3
"""
Record a Phantom session, then render the session to video at 60 fps.

    python tools/phantom_session.py record --port COM6 --out run.phr
    python tools/phantom_session.py render --port COM6 run.phr --dir .

A record restarts the game. A session must start at a state that the render step
can also start from. Play from the menu.

There are two steps because of the link. One frame is 460,800 bytes and the link
carries 0.74 MB/s, so the device cannot send 60 frames each second. The limit is
about 23. But the SIMULATION is one frame time and one input structure, which is
less than 100 bytes for each frame.

The record step saves only those. The cost is small, so the game keeps its full
speed while you play. The render step then runs the session again on the device,
one frame at a time, and reads the pixels at the speed of the link.

The video is 60 fps because the game made the frames 1/60 s apart. The render
step takes some minutes for each minute of play. This is a wait, not a loss of
quality: every frame is the true output of the rasteriser, and the fps counter
in the HUD shows the value it showed at the time.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import (AUDIO_RATE, Desync, FrameWriter, PhantomLink,
                          Session, reset_board, set_gamma)


def record(args):
    print("The device resets...")
    reset_board(args.port)
    link = PhantomLink(args.port)
    link.open()
    hdr = link.session_start()
    ses = Session(hdr)
    print(f"Recording. Play now. Press Ctrl+C to stop.  "
          f"(input structure {hdr['blob']} B, {len(hdr['seeds'])//4} seeds)")
    started = time.time()
    try:
        while True:
            ses.frames.append(link.session_frame())
            if len(ses.frames) % 30 == 0:
                print(f"\r  {len(ses.frames)} frames  {ses.seconds:6.1f} s of play"
                      f"  {len(ses.frames)/max(0.001, time.time()-started):5.1f} fps",
                      end="", flush=True)
    except KeyboardInterrupt:
        print("\nStopped.")
    except (Desync, TimeoutError) as e:
        print(f"\nThe link failed: {e}")
    finally:
        try:
            link.session_end()
        except Exception:
            pass
        link.close()

    if not ses.frames:
        sys.exit("No frames arrived. Select the port marked [ESP32].")
    ses.save(args.out)
    fps = len(ses.frames) / max(0.001, ses.seconds)
    print(f"Wrote {args.out}  ({len(ses.frames)} frames, {ses.seconds:.1f} s "
          f"of play at {fps:.1f} fps, {os.path.getsize(args.out)/1024:.0f} KB)")


def render(args):
    if args.gamma != 1.0:
        set_gamma(args.gamma)
        print(f"Gamma {args.gamma}. This matches the panel. It does not correct the capture.")
    # Check the folder first. Without this check the tool spends three minutes
    # on the render step and then fails at the first write.
    if not os.path.isdir(args.dir):
        sys.exit(f"The folder does not exist: {args.dir}")
    ses = Session.load(args.session)
    n = len(ses.frames)
    fps = n / max(0.001, ses.seconds)
    print(f"{n} frames, {ses.seconds:.1f} s of play at {fps:.1f} fps")

    print("The device resets...")
    reset_board(args.port)
    link = PhantomLink(args.port)
    link.open()
    link.replay_start(ses.hdr)   # waits until the device reports PLAYING

    # The video is written at 60 fps, which is the rate the game aims at. It is
    # NOT the average rate of the session. The writer puts each frame on this
    # grid at the frame's own true time, so a session that changed speed still
    # plays at the speed it ran at. An average rate cannot do that: it plays
    # every part of the session at the speed of the whole.
    writer = FrameWriter(args.dir, fps=60.0, fragmented=True)
    started = time.time()
    done = 0

    # The host always sends one entry more than the device asked for.
    #
    # A strict exchange of one entry for one frame makes the device idle between
    # frames. The last bytes of a frame then stay in the USB driver until more
    # traffic moves them. The host waited eight seconds for 100 bytes, and those
    # bytes arrived only after the host gave up and wrote something.
    #
    # With one entry in advance the device is never idle. It renders the next
    # frame while the host encodes the current frame. Two entries are enough:
    # six entries gave the same 19.5 fps, so the remaining cost is the link and
    # the render time of the device, not idle time.
    first = max(0, getattr(args, "first", 0))
    last  = getattr(args, "last", -1)
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
                print(f"\n  {e}. The host synchronises again.")
                continue
            # Time the frame by the sound that came with it. The device makes
            # one frame of sound for each frame of picture, so the samples are
            # the best measure of how long the frame lasted. Without sound, use
            # the time recorded in the session.
            chunk = bytes(link.audio)
            link.audio = bytearray()
            secs = (len(chunk) // 2) / AUDIO_RATE if chunk else fr["dt"]
            # OUTSIDE THE RANGE, SIMULATED BUT NOT KEPT.
            #
            # A record has to start where the render can start, which is the menu -- so every
            # session opens with the taps that got into a match, and footage of the flying part
            # has menus in front of it. The device still has to simulate the frames before the
            # range, because the state at frame N is the product of every frame before it, so
            # this cannot make the render faster. It only stops the frames reaching the video.
            if i >= first and (last < 0 or i <= last):
                writer.write(rgb, secs)
                writer.add_audio(chunk)
            done += 1
            el = time.time() - started
            rate = done / max(0.001, el)
            print(f"\r  {done}/{n}  {rate:5.1f} fps  "
                  f"{(n-done)/max(0.01, rate):5.0f} s left", end="", flush=True)
    except KeyboardInterrupt:
        print("\nStopped. The tool keeps the frames it rendered.")
    except TimeoutError as e:
        print(f"\nStopped early: {e}")
    finally:
        try:
            link.session_end()
        except Exception:
            pass
        link.close()
        path = writer.close()

    el = time.time() - started
    # writer.n is the count of frames in the file, which is not the count read
    # from the device: a frame can go in more than one time, or not at all.
    secs = writer.n / 60.0
    print(f"\nWrote {path}  ({done} frames read, {writer.n} written at 60 fps = "
          f"{secs:.1f} s of video in {el:.0f} s, "
          f"{el/max(0.001, secs):.1f} times the play time)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("record",
                       help="save a session while the game runs at full speed")
    r.add_argument("--port", required=True)
    r.add_argument("--out", default="session.phr")
    r.set_defaults(fn=record)

    p = sub.add_parser("render",
                       help="run a session again and read the pixels")
    p.add_argument("session")
    p.add_argument("--port", required=True)
    p.add_argument("--dir", default=".")
    # Which frames reach the video. The whole session is still simulated -- see the note in
    # the render loop -- so this trims the menus off the front rather than saving any time.
    p.add_argument("--from", dest="first", type=int, default=0,
                   help="first frame to keep (default 0)")
    p.add_argument("--to", dest="last", type=int, default=-1,
                   help="last frame to keep, -1 for the end")
    p.add_argument("--gamma", type=float, default=1.0,
                   help="1.0 is the default and keeps the values of the "
                        "framebuffer. A larger value makes the dark parts "
                        "brighter, as the AMOLED panel shows them.")
    p.set_defaults(fn=render)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
