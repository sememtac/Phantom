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
import phantom_link
from phantom_link import Desync, PhantomLink, Session, reset_board

# Spread through a session rather than bunched: the cost of a run is set by the
# DEEPEST frame, because the device must replay everything before it.
# These run out to 16,000 ON PURPOSE: they are sized for the anomaly session, which is
# 8,442 frames, not for regress.phr's 5,444. Against the short session the tail simply
# does not exist -- which is fine, and used to be SILENT, which was not. See render().
DEFAULT_FRAMES = "500,1840,3400,4760,6500,7700,10000,13000,16000"


# THE TREE, NOT JUST THE COMMIT.
#
# `git rev-parse HEAD` describes the last commit, not the build that was flashed, and three
# separate measurements this week recorded a commit that did not contain what was measured:
# a baseline stamped 32d84b6 for a build containing the surge, and a cost comparison that
# printed "b9ad6ec -> b9ad6ec" because both runs read HEAD while the work sat uncommitted.
#
# Each one looks authoritative and names a build that never existed. A `+dirty` suffix is
# not precise -- it cannot say WHICH changes -- but it is honest, and it is the difference
# between a number you can trust and one you have to remember the provenance of.
def git_commit():
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        h = out.stdout.strip()
        if not h:
            return "unknown"
        st = subprocess.run(["git", "status", "--porcelain", "--", "src"],
                            capture_output=True, text=True, timeout=10)
        return h + ("+dirty" if st.stdout.strip() else "")
    except Exception:
        return "unknown"


# WHAT THE DEVICE SAYS WHEN THE RENDER STOPS, and this tool used to throw it away.
#
# vg_replay_command('E') prints RESYNC, WHY and END. The END line carries the
# device's own write counts: bytes, short, stall, mismatch. Those counts are the
# only evidence that says which end of the link lost the bands. Without them a
# checksum failure is equally explained by a device that under-wrote and a host
# that under-read, and the two need different fixes.
#
# replay_cost.py learned the same lesson: whichever end explains a failure must
# be the end that waits longer. This one sent EEEE and closed the port in the
# next statement, so the report was always made after its reader had gone.
def device_report(link, wait=8.0):
    """Read the device's end-of-replay report. Returns the lines it printed."""
    # The stream is still binary here: the device finishes the frame it is in
    # before it reads the EEEE tag. So take the whole buffer and keep the text.
    seen = bytearray()
    deadline = time.time() + wait
    while time.time() < deadline:
        with link._rx_lock:
            seen += link._rx
            del link._rx[:]
        if b"vg_replay: END" in seen:
            break
        time.sleep(0.05)
    return [ln.strip() for ln in seen.decode("utf-8", "replace").splitlines()
            if "vg_replay:" in ln]


def render(port, path, wanted, out_dir):
    # NUMPY IS NOT OPTIONAL HERE. phantom_link falls back to plain Python lists
    # without it, and that path decodes a band slowly enough to starve the receive
    # thread: measured on 2026-09-06, more than 220 bands lost in 1,200 frames under
    # a Python with no numpy, against 6 with it. The loss looks like a link fault
    # and it is not one. Refuse, and say which Python this is.
    if phantom_link._np is None:
        sys.exit("phantom_regress: numpy is not installed for " + sys.executable + ".\n"
                 "  Without it the render loses bands and reports the link as corrupt.\n"
                 "  Run this with a Python that has numpy (pip install numpy).")
    ses = Session.load(path)
    n = len(ses.frames)
    # SAY WHAT WAS DROPPED. A frame past the end of the session cannot be rendered, and
    # this used to filter it out without a word -- so a run that asked for nine frames and
    # tested four reported success in the same voice as a run that tested nine.
    # regress-baseline-exact.json is exactly that: four hashes, saved as a nine-frame test.
    dropped = [f for f in wanted if f >= n]
    wanted = [f for f in wanted if f < n]
    if dropped:
        print(f"  NOTE: {len(dropped)} of {len(dropped) + len(wanted)} frames are past the "
              f"end of this {n}-frame session and will not be tested: "
              + ",".join(str(f) for f in dropped))
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
    # NO AUDIO. This tool hashes pixels and throws the samples away one line
    # after it reads them. The device sends about 730 bytes of sound with every
    # frame, which is 3% of the run, and the link corrupts at a rate for each
    # byte. Bytes nobody reads can still lose a band. See replay_cost.py, which
    # asks for no audio for the same reason.
    link.replay_start(ses.hdr, audio=False)

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
        said = []
        try:
            link.session_end()
            said = device_report(link)
        except Exception:
            pass
        link.close()

    if desyncs:
        print(f"\n{desyncs} band(s) failed to decode during this run. The link "
              f"corrupted; the numbers below are only as good as that.")
        for line in said:
            print(f"  device: {line}")
        print("  Read `stall` above. It counts the writes the device could not "
              "place because the host was not reading. The device only drops "
              "bytes after 3000 of them in one band, so a small `stall` means "
              "the device sent every byte and the HOST lost them.")
    if bad:
        print(f"frames that could not be hashed: {bad}")
    return got, bad


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
    got, bad = render(args.port, args.session, wanted, args.dir)

    # A BASELINE WITH A HOLE IN IT IS PERMANENT.
    #
    # A frame the link corrupted is not in `got`, so the file used to be written
    # without it and with no error. --against then takes its frame list FROM the
    # baseline, so the missing frame is never asked for again and every later run
    # prints "all N frames identical" over a corpus nobody chose. One bad band
    # therefore deleted a frame from the test for good.
    #
    # This is the third time this tool has counted the wrong thing. Re-run
    # instead: corruption lands on a different frame every time.
    if args.save and bad:
        sys.exit(f"\nNO BASELINE WRITTEN. The link corrupted {len(bad)} of the "
                 f"{len(wanted)} wanted frames: {bad}. A baseline that is short "
                 f"of a frame drops it from the test for ever. Run it again.")

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
        # `lost` are the frames the link corrupted. They are not in `got` at all,
        # so a comparison that ignores them checks fewer frames than it was asked
        # for and still reports a pass.
        lost = [k for k in bad if str(k) in old]
        diff = 0
        same = 0
        for k in sorted(got, key=int):
            a = old.get(str(k))
            b = got[k]
            if a is None:
                print(f"  {k:6d}  not in the baseline")
            elif a != b:
                print(f"  {k:6d}  DIFFERENT")
                diff += 1
            else:
                same += 1
        if diff:
            print(f"\n{diff} of {diff + same} frames changed. If this change was "
                  f"meant to keep the picture identical, it did not.")
            return 1
        # Count the frames that were really COMPARED, not the frames rendered. A
        # frame the baseline does not hold proves nothing, and a run where none
        # of them matched up used to report a clean result.
        if not same:
            print("\nNOTHING WAS COMPARED: no rendered frame is in the baseline. "
                  "This is not a pass.")
            return 1
        # A frame the link ate is a frame this run did not check. Saying
        # "identical" about the rest is true and is not the answer that was
        # asked for, so it does not pass.
        if lost:
            print(f"\n{same} frames identical, but the link corrupted {len(lost)} "
                  f"more that the baseline holds: {lost}. THIS IS NOT A PASS. "
                  f"Run it again; corruption moves to a different frame.")
            return 1
        print(f"\nall {same} frames identical.")
        return 0

    print("\nno --save and no --against, so nothing was compared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
