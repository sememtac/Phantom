#!/usr/bin/env python3
"""Measure what the frame costs, over a recorded session instead of a live fight.

    python tools/replay_cost.py captures/regress.phr --port COM6
    python tools/replay_cost.py captures/regress.phr --port COM6 --save a.json
    python tools/replay_cost.py captures/regress.phr --port COM6 --against a.json

The device replays the session frame for frame and reports the CPU it spent, and
sends no pixels. A run takes about half a minute.

WHY THIS EXISTS. The obvious way to measure a drawing is to fly with it and read the
telemetry. That does not work, and this project proved it three times in one day. The
`can` counter was measured swinging 2.8x between two moments of the SAME fight in the
SAME build, so a before-and-after taken from two live fights reports whatever the two
moments happened to be doing. One such comparison said a drawing cost 60% more when it
covered 4.5% more; the scene-independent measure said 7%.

A replay cannot move. Same seeds, same input, same frames, so the only difference
between two runs is what changed in the build.

WHAT IT DOES NOT MEASURE. Frame rate. With no pixels going out, the panel is never the
bottleneck, so the frame time here is not the frame time a player sees. These are CPU
microseconds: the work, not the pipeline. To learn whether the work FITS, compare the
`rast` figure with the wire floor of 11520 us -- under it, more work is largely absorbed
by the transfer wait; over it, every microsecond is on the frame.

    can     the cockpit
    rast    the whole raster: sky + primitives + scanlines
    prim    the primitives alone
    sub     building the primitive list
    upd     the simulation

Close every other program that uses the port first.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

from phantom_link import Desync, PhantomLink, Session, open_quiet, reset_board

COST = re.compile(
    r"vg_replay: COST frames (\d+) \| can (\d+)/(\d+) \| rast (\d+)/(\d+) \| "
    r"prim (\d+)/(\d+) \| sub (\d+)/(\d+) \| upd (\d+)/(\d+)")

KEYS = ["can", "rast", "prim", "sub", "upd"]
WIRE_US = 11520          # 460,800 bytes at 80 MHz quad. See cfg_display.h.


def git_commit():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


def run(port, path, limit):
    ses = Session.load(path)
    n = len(ses.frames)
    if limit and limit < n:
        n = limit
    print("%s: %d frames, timed replay (no pixels)" % (os.path.basename(path), n))

    reset_board(port, settle=6.0)
    link = PhantomLink(port)
    link.open()

    # 'T' rather than 'P', through the SAME handshake. Hand-rolling it here failed
    # silently -- the device answered nothing at all, and so did a hand-rolled 'P',
    # which is what proved the copy rather than the new command was at fault.
    # No audio: nobody is going to listen to a run with no pictures.
    try:
        link.replay_start(ses.hdr, audio=False, cmd=b"T")
    except Desync as e:
        sys.exit("%s\n  Record a new session: sizeof(VgInput) probably changed." % e)

    # SENT is not RUN, and the difference is the whole timing of this tool.
    #
    # The host writes every record in under a second; the device then works through them
    # at the rate it can actually draw frames, which is tens of times slower. So the
    # progress below counts what has been HANDED OVER and deliberately claims no rate --
    # the first version printed the host's write rate, 3,669 fps, which is a real number
    # measuring nothing anybody wants.
    #
    # 'E' goes into the same stream behind the records, so it arrives in order and ends
    # the run at the right frame rather than cutting it short.
    # A WINDOW, NOT THE WHOLE SESSION. The device answers each frame with one byte; every
    # answer buys the right to send one more record. Sending them all at once overruns the
    # device's receive ring and desyncs the record stream -- see the note in main.cpp, where
    # the failure was a good deal stranger than a lost byte.
    #
    # Sixteen deep so the device never waits on the wire, which is what keeps the run at the
    # rate it can draw rather than at the round trip.
    # THE ACKS PACE IT; THEY DO NOT GATE IT, and that difference is the whole reliability
    # of this loop.
    #
    # Counting acks as permission to send exactly one more deadlocks the moment the count
    # drifts by one in the device's favour: the host waits for an ack, the device waits for
    # a record, and neither ever moves. That is what happened, and the giveaway was that it
    # stalled at 245 frames one run and 353 the next -- a bad frame stalls at the same
    # number every time, and a race does not.
    #
    # So a quiet link is not an error. If nothing comes back for a moment and there are
    # records left to send, send one: the device is either busy or waiting, and one more
    # record in its ring is harmless either way. The window still does the pacing, the acks
    # still keep it full, and no arithmetic between them can wedge it.
    WINDOW = 16
    link.ser.timeout = 0.5
    started = time.time()
    sent = done = 0
    quiet = 0
    while sent < min(WINDOW, n):
        link.replay_send(ses.frames[sent]); sent += 1
    while done < n:
        b = link.ser.read(1)
        if b:
            quiet = 0
            done += len(b)
        else:
            quiet += 1
            # 30 s of complete silence with nothing outstanding is a dead device, not a
            # slow one: a frame takes tens of milliseconds.
            if quiet > 60 and sent >= n:
                sys.exit("the device stopped answering after %d of %d frames.\n"
                         "  It reset, or the replay desynced." % (done, n))
            if sent < n:
                link.replay_send(ses.frames[sent]); sent += 1
                continue
        while sent < n and sent - done < WINDOW:
            link.replay_send(ses.frames[sent]); sent += 1
        if done and done % 1000 == 0:
            rate = done / max(time.time() - started, 1e-6)
            print("  ...%d/%d  %.0f fps  %.0fs left"
                  % (done, n, rate, (n - done) / max(rate, 1e-6)))
    print("  device drew %d frames in %.1fs" % (n, time.time() - started))
    link.ser.timeout = 8

    # FOUR BYTES, NOT ONE, and this is why the tool appeared to hang at the end.
    #
    # A replay in progress is not reading commands: vg_replay_next owns the stream and reads
    # a four-byte record tag. A lone 'E' is one quarter of a tag, so the device sat in that
    # read for its full 30-second timeout before deciding the session was over -- and only
    # then printed the END and COST lines. The host gave up at 20 seconds and reported that
    # the device had said nothing, while the numbers were about to arrive.
    #
    # Any four bytes that are not "PHRP" end it immediately; the mismatch is what the device
    # tests, not the content. 'E' four times says what it means to a human reading the wire.
    # END IT ON THE HANDLE THAT RAN IT, then ASK for the answer on a new one.
    #
    # Four bytes, because a replay in progress is not reading commands: vg_replay_next owns
    # the stream and reads a four-byte record tag, so a lone 'E' left the device waiting out
    # a 30-second timeout before it would end.
    link.ser.write(b"EEEE")
    link.ser.flush()
    time.sleep(0.5)
    link.close()

    # A NEW HANDLE, BECAUSE THE OLD ONE IS NOT SAFE TO READ. Reading the tail on the
    # connection that carried nine hundred acknowledgements killed the interpreter with an
    # access violation, byte at a time and in bulk alike.
    #
    # Windows does not release a USB CDC port instantly, and reopening too soon crashed the
    # same way -- so this waits and retries rather than assuming.
    ser = None
    for attempt in range(8):
        time.sleep(0.6)
        try:
            ser = open_quiet(port, timeout=0.5)
            break
        except Exception:
            continue
    if ser is None:
        sys.exit("could not reopen %s to collect the answer.\n"
                 "  The run itself finished; ask the device yourself with:\n"
                 "    python tools/listen.py %s   (then press c)" % (port, port))

    # ASKED FOR, NOT CAUGHT. The device keeps the last timed run's sums until the next one,
    # so this cannot race with a reconnect -- which is what lost the answer every previous
    # time, USB CDC having discarded it while no host was attached.
    ser.write(b"c")
    ser.flush()

    # Short, because the device answers a command in a frame or two. Nothing is being
    # waited for here except one printf.
    deadline = time.time() + 10.0
    tail = bytearray()
    while time.time() < deadline:
        k = ser.in_waiting
        if not k:
            time.sleep(0.02)
            continue
        tail += ser.read(k)
        m = COST.search(tail.decode("utf-8", "replace"))
        if m:
            ser.close()
            g = [int(x) for x in m.groups()]
            out = {"frames": g[0], "commit": git_commit(),
                   "session": os.path.basename(path)}
            for j, k in enumerate(KEYS):
                out[k] = {"mean": g[1 + j * 2], "worst": g[2 + j * 2]}
            return out
    ser.close()
    # SHOW WHAT IT DID SAY. A tool that reports only "no cost line" sends the reader to
    # look at the firmware version, which was wrong both times it happened here.
    said = tail.decode("utf-8", "replace").strip()
    sys.exit("the device ran the session but reported no cost line.\n"
             "  It said:\n    %s"
             % ("\n    ".join(said.splitlines()[-6:]) if said else "(nothing at all)"))


def show(r):
    print("\n%d frames, commit %s" % (r["frames"], r["commit"]))
    print("  %-6s %10s %10s" % ("", "mean us", "worst us"))
    for k in KEYS:
        print("  %-6s %10d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    rast = r["rast"]["mean"]
    room = WIRE_US - rast
    if room >= 0:
        print("\n  rast is %d us under the %d us wire floor: there is that much raster\n"
              "  headroom before more work starts costing frames." % (room, WIRE_US))
    else:
        print("\n  rast is %d us OVER the %d us wire floor: the raster sets the frame\n"
              "  now, so every extra microsecond of drawing is one on the frame."
              % (-room, WIRE_US))


def compare(now, was):
    print("\n%-6s %12s %12s %12s" % ("", was["commit"], now["commit"], "change"))
    for k in KEYS:
        a, b = was[k]["mean"], now[k]["mean"]
        d = b - a
        pct = (100.0 * d / a) if a else 0.0
        print("  %-6s %10d %12d %9d %+6.1f%%" % (k, a, b, d, pct))
    if was.get("session") != now.get("session"):
        print("\n  DIFFERENT SESSIONS (%s vs %s). The whole point of this tool is that\n"
              "  both runs see the same frames, so this comparison means nothing."
              % (was.get("session"), now.get("session")))
    if was["frames"] != now["frames"]:
        print("\n  Different frame counts (%d vs %d): one run ended early, so the means\n"
              "  are over different work." % (was["frames"], now["frames"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--port", required=True)
    ap.add_argument("--frames", type=int, default=0,
                    help="stop after this many frames (default: the whole session)")
    ap.add_argument("--save", metavar="FILE", help="write the result")
    ap.add_argument("--against", metavar="FILE", help="compare with a saved result")
    a = ap.parse_args()

    r = run(a.port, a.session, a.frames)
    show(r)

    if a.against:
        with open(a.against) as fh:
            compare(r, json.load(fh))
    if a.save:
        with open(a.save, "w") as fh:
            json.dump(r, fh, indent=2)
        print("\nwrote %s" % a.save)

    # LEAVE WITHOUT UNWINDING, and this is not tidiness either.
    #
    # Every run of this tool crashed the interpreter on the way out -- an access violation,
    # after the report had been printed and the file written, so the work was done and the
    # exit code said 3221225477. A tool nothing can check the exit code of is a tool nothing
    # can put in a script.
    #
    # It is a teardown fault in the serial layer on a port that carried nine hundred
    # acknowledgements, not anything this file computes. Both handles are already closed by
    # here. So: say what happened, make sure it is on the terminal, and go.
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
