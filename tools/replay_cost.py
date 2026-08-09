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

# The `world` split, on its own line. Optional, so a device built before this existed
# still reports the first line and a run against one degrades rather than fails.
WORLD = re.compile(
    r"vg_replay: WORLD motes (\d+)/(\d+) \| rocks (\d+)/(\d+) \| trails (\d+)/(\d+) \| "
    r"ships (\d+)/(\d+) \| ord (\d+)/(\d+)")

KEYS = ["can", "rast", "prim", "sub", "upd"]
WKEYS = ["motes", "rocks", "trails", "ships", "ord"]
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

    # SEND A BATCH, THEN DRAIN ITS ANSWERS. Never both at once.
    #
    # Three earlier versions interleaved reads and writes -- an acknowledgement read between
    # every record sent -- and every one of them died above about a thousand frames with an
    # access violation inside pyserial. Batching the writes helped and did not fix it;
    # batching the reads moved the failure rather than removing it. What they had in common
    # was reading a Windows COM port while writes were still in flight on it.
    #
    # So this alternates strictly. Send sixteen records, then wait for sixteen bytes back,
    # then send sixteen more. There is no moment when a read and a write overlap.
    #
    # SIXTEEN IS THE DEVICE'S RECEIVE RING, not a tuning knob. That ring is 2048 bytes and a
    # record is 89, so at most 23 can be outstanding; sending more silently drops bytes and
    # the device then reads a record tag that is not "PHRP" and quietly ends the replay. That
    # failure looked like the device dying at frame 244, and was the host shouting over it.
    #
    # The cost is that the device idles briefly between batches, so a run takes a little
    # longer than the frames themselves do. That is the whole price of never crashing.
    CHUNK = 16
    link.ser.timeout = 1.0
    started = time.time()
    sent = shown = 0

    while sent < n:
        k = min(CHUNK, n - sent)
        for i in range(k):
            link.replay_send(ses.frames[sent + i])
        sent += k

        # Exactly k bytes back, one per frame drawn. A short read is the device still
        # working, not an error -- only a long silence is.
        got = 0
        quiet = 0
        while got < k:
            b = link.ser.read(k - got)
            if b:
                got += len(b)
                quiet = 0
            else:
                quiet += 1
                # 20 s with nothing at all, when a frame takes tens of milliseconds.
                if quiet > 20:
                    sys.exit("the device stopped answering after %d of %d frames.\n"
                             "  It reset, or the replay desynced." % (sent - k + got, n))
        if sent - shown >= 1000:
            shown = sent
            rate = sent / max(time.time() - started, 1e-6)
            print("  ...%d/%d  %.0f fps  %.0fs left"
                  % (sent, n, rate, (n - sent) / max(rate, 1e-6)))

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

    # COLLECTED BY A SEPARATE PROCESS, not here. See fetch() for why -- in short, this
    # handle has just carried eleven thousand operations and cannot be read afterwards.
    link.close()
    return None


def fetch(port):
    """Ask the device for the last timed run's cost, on a fresh connection.

    SEPARATE FROM THE RUN, because a handle that has carried a full session cannot be
    trusted to read afterwards. 5,444 records out and 5,444 acknowledgements back is about
    eleven thousand operations on one Windows COM port, and past roughly a thousand frames
    the tail read comes back empty and the interpreter dies on exit. Closing and reopening
    inside the same process crashes too.

    A fresh PROCESS is the one thing that reliably works, so the device keeps the answer --
    see vg_replay_report_cost -- and this goes and gets it.

    IT MUST FOLLOW THE RUN IMMEDIATELY, which is why the run invokes it rather than leaving
    it to the reader. Opening this port resets the board unless it was open moments ago, and
    a reset clears the sums -- so `--fetch` on its own, minutes later, reliably answers
    "no timed run yet" about a run that definitely happened. Straight after the parent closes
    the port, the open does not reset and the answer is there.
    """
    ser = open_quiet(port, timeout=0.5)
    ser.reset_input_buffer()
    ser.write(b"c")
    ser.flush()
    deadline = time.time() + 10.0
    tail = bytearray()
    while time.time() < deadline:
        k = ser.in_waiting
        if not k:
            time.sleep(0.02)
            continue
        tail += ser.read(k)
        txt = tail.decode("utf-8", "replace")
        m = COST.search(txt)
        # WAIT FOR BOTH LINES. The device prints WORLD after COST, so returning the moment
        # COST matched collected the frame costs and silently dropped the world split --
        # which is the half this was extended for.
        if m and (WORLD.search(txt) or time.time() > deadline - 8.0):
            ser.close()
            g = [int(x) for x in m.groups()]
            out = {"frames": g[0], "commit": git_commit(), "session": "(fetched)"}
            for j, k2 in enumerate(KEYS):
                out[k2] = {"mean": g[1 + j * 2], "worst": g[2 + j * 2]}
            w = WORLD.search(txt)
            if w:
                gw = [int(x) for x in w.groups()]
                for j, k2 in enumerate(WKEYS):
                    out[k2] = {"mean": gw[j * 2], "worst": gw[j * 2 + 1]}
            return out
    ser.close()
    said = tail.decode("utf-8", "replace").strip()
    sys.exit("the device reported no cost line.\n  It said:\n    %s"
             % ("\n    ".join(said.splitlines()[-4:]) if said else "(nothing at all)"))


def show(r):
    print("\n%d frames, commit %s" % (r["frames"], r["commit"]))
    print("  %-6s %10s %10s" % ("", "mean us", "worst us"))
    for k in KEYS:
        print("  %-6s %10d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if all(k in r for k in WKEYS):
        print("  -- inside `world`; the WORST column is the one that matters --")
        for k in WKEYS:
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
    ap.add_argument("--fetch", action="store_true",
                    help="do not run; just read the last run's result off the device")
    a = ap.parse_args()

    if a.fetch:
        r = fetch(a.port)
    else:
        run(a.port, a.session, a.frames)
        # A FRESH PROCESS FOR THE ANSWER. Re-running this file with --fetch is the only
        # thing that reliably reads a device after a full session; doing it in-process
        # crashes, whether on the same handle or a reopened one.
        print("  collecting the result...")
        argv = [sys.executable, os.path.abspath(__file__),
                a.session, "--port", a.port, "--fetch"]
        # --save and --against belong to the FETCH, which is the half that has the numbers.
        # Left off, a full run wrote no baseline and said nothing about it.
        if a.save:
            argv += ["--save", a.save]
        if a.against:
            argv += ["--against", a.against]
        out = subprocess.run(argv, capture_output=True, text=True)
        sys.stdout.write(out.stdout)
        if out.returncode != 0:
            sys.stderr.write(out.stderr)
            sys.exit(out.returncode)
        return
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
