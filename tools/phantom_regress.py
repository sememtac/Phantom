#!/usr/bin/env python3
"""Render fixed frames of a session and hash them, to prove a change is safe.

    python tools/phantom_regress.py --port COM6 run.phr --save base.json
    python tools/phantom_regress.py --port COM6 run.phr --against base.json

The replay is the only regression test this project has. A session renders frame
for frame, so if the same frames come back with the same bytes, neither the
simulation nor the drawing changed.

Use it like this:

1. Take a baseline before you start.
2. Make a change that must not alter the picture -- moving code between files,
   deleting code that never ran.
3. Compare. Any difference is a fault.

A change that is MEANT to alter the picture cannot use this. Take a new baseline
after it, and keep the two kinds of change in separate commits.

The device must be idle. A render holds the port, and a render that is stopped
part way leaves the device reading every byte as session data -- it needs a reset
before the next one, which this program does at the start of every run.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from phantom_link import Desync, PhantomLink, Session, reset_board

# Spread through a session rather than bunched: the cost of a run is set by the
# DEEPEST frame, because the device must replay everything before it.
DEFAULT_FRAMES = "500,1840,3400,4760,6500,7700,10000,13000,16000"


def git_commit():
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def render(port, path, wanted, out_dir):
    ses = Session.load(path)
    n = len(ses.frames)
    wanted = [f for f in wanted if f < n]
    if not wanted:
        sys.exit(f"no wanted frame is inside this session ({n} frames)")
    last = max(wanted)
    print(f"{os.path.basename(path)}: {n} frames, rendering to {last}")

    # Longer than the default settle. Boot now builds the menu backdrop, and a
    # 'P' that arrives before the device is reading commands is simply lost --
    # which looks exactly like a dead link, because no frames ever come.
    reset_board(port, settle=6.0)
    link = PhantomLink(port)
    link.open()
    link.replay_start(ses.hdr)

    DEPTH = 2
    for fr in ses.frames[:DEPTH]:
        link.replay_send(fr)

    got = {}
    bad = []          # wanted frames that could not be decoded
    desyncs = 0       # every bad band, wanted or not: the corruption RATE
    started = time.time()
    try:
        for i in range(n):
            nxt = i + DEPTH
            if nxt < n:
                link.replay_send(ses.frames[nxt])
            try:
                rgb, w, h = link.read_frame()
            except Desync as e:
                desyncs += 1
                # A frame we were going to hash is worse than one we were not.
                # Hashing a frame the decoder had to guess at is how three runs
                # of one session produced three answers and looked like a
                # simulation that was not reproducible.
                if i in wanted:
                    bad.append(i)
                    print(f"  {i:6d}  UNUSABLE: {e}")
                else:
                    print(f"  frame {i}: {e}")
                continue
            link.audio = bytearray()
            # FROM HERE ON, i IS THE DEVICE'S FRAME NUMBER, not this loop's count.
            #
            # A frame skipped for a bad band still advances the counter, so a
            # single transient corruption shifted host and device apart by one and
            # every later comparison was then against the wrong frame. It reported
            # six of eight frames changed for a commit that only deleted code that
            # never ran, which is precisely the false alarm this harness exists to
            # not raise.
            i = getattr(link, "last_idx", i)
            if i in wanted:
                print(f"      host {i} = device frame {getattr(link, 'last_idx', -1)}")
                data = bytes(rgb)
                got[i] = hashlib.sha256(data).hexdigest()
                if out_dir:
                    with open(os.path.join(out_dir, f"f{i:06d}.ppm"), "wb") as fh:
                        fh.write(b"P6\n%d %d\n255\n" % (w, h))
                        fh.write(data)
                print(f"  {i:6d}  {got[i][:16]}")
            if i % 200 == 0:
                el = time.time() - started
                rate = (i + 1) / max(0.001, el)
                print(f"  ...{i}/{last}  {rate:4.1f} fps  "
                      f"{(last - i) / max(0.01, rate):4.0f}s left")
            if i >= last:
                break
    finally:
        try:
            link.session_end()
        except Exception:
            pass
        link.close()

    if desyncs:
        print(f"\n{desyncs} band(s) failed to decode during this run. The link "
              f"corrupted; the numbers below are only as good as that.")
    if bad:
        print(f"frames that could not be hashed: {bad}")
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--port", required=True)
    ap.add_argument("--frames", default=None,
                    help="comma separated frame numbers. With --against, the "
                         "default is the frames the baseline holds")
    ap.add_argument("--save", metavar="FILE", help="write a baseline")
    ap.add_argument("--against", metavar="FILE", help="compare with a baseline")
    ap.add_argument("--dir", help="also write each frame as a .ppm here")
    args = ap.parse_args()

    if args.dir:
        os.makedirs(args.dir, exist_ok=True)

    # The frames come from the baseline file, for --against and for --save over a
    # file that already exists. Passing --frames as well is allowed, but the
    # default must not be the built-in list: a comparison against a baseline
    # built from other frames compared nothing at all and still printed
    # "identical", and RETAKING a baseline would have quietly swapped the corpus
    # for a different one, which is worse -- every later run would then agree
    # with a set of frames nobody chose.
    spec = args.frames
    ref = args.against or (args.save if args.save and os.path.exists(args.save) else None)
    if spec is None and ref:
        with open(ref) as fh:
            spec = ",".join(sorted(json.load(fh)["frames"], key=int))
        print(f"frames from {ref}: {spec}")
    wanted = sorted(int(x) for x in (spec or DEFAULT_FRAMES).split(",") if x.strip())
    got = render(args.port, args.session, wanted, args.dir)

    if args.save:
        with open(args.save, "w") as fh:
            json.dump({"commit": git_commit(),
                       "session": os.path.basename(args.session),
                       "frames": {str(k): v for k, v in sorted(got.items())}},
                      fh, indent=2)
        print(f"\nbaseline written to {args.save}  (commit {git_commit()})")
        return 0

    if args.against:
        with open(args.against) as fh:
            base = json.load(fh)
        old = base["frames"]
        print(f"\nbaseline commit {base['commit']}, now {git_commit()}")
        bad = 0
        same = 0
        for k in sorted(got, key=int):
            a = old.get(str(k))
            b = got[k]
            if a is None:
                print(f"  {k:6d}  not in the baseline")
            elif a != b:
                print(f"  {k:6d}  DIFFERENT")
                bad += 1
            else:
                same += 1
        if bad:
            print(f"\n{bad} of {bad + same} frames changed. If this change was "
                  f"meant to keep the picture identical, it did not.")
            return 1
        # Count the frames that were really COMPARED, not the frames rendered. A
        # frame the baseline does not hold proves nothing, and a run where none
        # of them matched up used to report a clean result.
        if not same:
            print("\nNOTHING WAS COMPARED: no rendered frame is in the baseline. "
                  "This is not a pass.")
            return 1
        print(f"\nall {same} frames identical.")
        return 0

    print("\nno --save and no --against, so nothing was compared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
