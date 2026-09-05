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

    --warp flat | full | rigid
            Hold the cockpit's throttle bend at none, or at full, for the whole run.
            Without it the frame bends as the recorded throttle drives it. Run the
            same session once each way to measure what the stretch costs. Only
            `can` can move between those two runs. `rigid` also holds the lag off,
            so the frame does not move at all.

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
# The blit split, on its own line, and optional for the same reason WORLD is: a device
# built before it existed still answers the first two lines.
BLIT = re.compile(
    r"vg_replay: BLIT join (\d+)/(\d+) \| wait (\d+)/(\d+) \| push (\d+)/(\d+) \| "
    r"res (\d+)/(\d+) \| overn (\d+)/(\d+) \| overus (\d+)/(\d+) \| "
    r"sky (\d+)/(\d+) \| scan (\d+)/(\d+) \| idle0 (\d+)/(\d+)")
BKEYS = ["join", "wait", "push", "res", "over_n", "over_us", "sky", "scan", "idle0"]

# Per band, and the window they are measured against. A band under it costs nothing at
# all, so this line is read as "which of these are over", not as a profile.
BANDS = re.compile(r"vg_replay: BANDS/(\d+) = ([0-9 ]+)")

# The submit split. `wait` is the rendezvous gap -- core 1 idle while core 0 finishes.
SUB = re.compile(
    r"vg_replay: SUB a (\d+)/(\d+) \| b (\d+)/(\d+) \| wait (\d+)/(\d+) \| "
    r"arena (\d+)/(\d+) \| star (\d+)/(\d+) \| hud (\d+)/(\d+)")
# `sub_wait`, not `wait`: BKEYS carries a `wait` of its own and both lines land in
# one flat dict, so the blit's wire wait quietly overwrote this one and the SUB table
# printed it under "core 1 idle at the rendezvous". The two are unrelated numbers and
# the report showed the same value twice for weeks. Any rendezvous figure recorded
# before 2026-08-20 is the blit wait and has to be re-measured.
SKEYS = ["A(world)", "B(instr)", "sub_wait", "arena", "star", "hud"]

TYPES = re.compile(
    r"vg_replay: TYPES aa (\d+)/(\d+) \| ln (\d+)/(\d+) \| tri (\d+)/(\d+) \| "
    r"gl (\d+)/(\d+) \| fl (\d+)/(\d+)")
TKEYS = ["aa", "ln", "tri", "gl", "fl"]

WORLD = re.compile(
    r"vg_replay: WORLD motes (\d+)/(\d+) \| rocks (\d+)/(\d+) \| trails (\d+)/(\d+) \| "
    r"ships (\d+)/(\d+) \| msl (\d+)/(\d+) \| fire (\d+)/(\d+) \| TOTAL (\d+)/(\d+)")

KEYS = ["can", "rast", "prim", "sub", "upd"]
# The dips. The device folds each frame into a frame time -- upd + sub + max(rast,
# wire) -- and counts the ones a pilot would feel. These are the numbers a change
# meant to stop the combat dip is judged by; a mean cannot see a dip.
SLOWEST = re.compile(r"vg_replay: SLOWEST (\d+) us .*\| (\d+) frames under 60, (\d+) under 50")
HIST = re.compile(r"vg_replay: FRAMES 60\+ (\d+) \| 57-60 (\d+) \| 54-57 (\d+) \| 50-54 (\d+) \| under50 (\d+)")
SLOW = re.compile(r"vg_replay: SLOW(\d) frame (\d+)  (\d+) us \((\d+) fps\)  upd (\d+) sub (\d+) rast (\d+)"
                  r" \(can (\d+) prim (\d+) scan (\d+) tv (\d+)\)(?: \| A (\d+) B (\d+) wait (\d+) sxr (\d+) world (\d+))?")
SLOWK = ["i", "frame", "us", "fps", "upd", "sub", "rast", "can", "prim", "scan", "tv",
         "A", "B", "wait", "sxr", "world"]
# The picture hash: every Nth frame's bands folded on the device. Two builds that
# draw the same pixels give the same word; the timings of a hashed run are polluted
# by the fold and are not to be compared.
BANDH = re.compile(r"vg_replay: BANDH ([0-9a-f]{8}) over (\d+) bands, 1 frame in (\d+)")
# The canopy by core. `can` is the slower half a band; these are both halves.
CAN = re.compile(r"vg_replay: CAN c0 (\d+) \| c1 (\d+) \| at (\d+)")
WKEYS = ["motes", "rocks", "trails", "ships", "msl", "fire", "TOTAL"]
WIRE_US = 11520          # 460,800 bytes at 80 MHz quad. See cfg_display.h.


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
        h = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                    stderr=subprocess.DEVNULL).decode().strip()
        if not h:
            return "unknown"
        dirty = subprocess.check_output(["git", "status", "--porcelain", "--", "src"],
                                        stderr=subprocess.DEVNULL).decode().strip()
        return h + ("+dirty" if dirty else "")
    except Exception:
        return "unknown"


def choose_canopy(port, delta):
    """Tell the board which cockpit to fly, and REFUSE TO GO ON unless it says so.

    WHY THIS IS NOT JUST A WRITE. It was, and the pair of runs it produced were
    identical to the microsecond -- can 5894 both ways, every scene counter equal.
    That reads as "the two cockpits cost the same", which would be a finding. It
    was not one: the command had not taken, and both runs flew the same cockpit.

    A measurement that silently compares a thing against itself is worse than one
    that fails, because it answers. So this waits for the device to name the
    cockpit back and stops the run if it does not.

    Sent on a plain handle before PhantomLink opens, because the link runs a reader
    thread of its own and the acknowledgement has to be read by somebody.
    """
    want = "DELTA" if delta else "OPAQUE"
    ser = open_quiet(port)
    try:
        time.sleep(0.5)
        ser.reset_input_buffer()
        for _ in range(4):
            ser.write(b"O" if delta else b"o")
            ser.flush()
            t0, buf = time.time(), ""
            while time.time() - t0 < 1.5:
                buf += ser.read(4096).decode("ascii", "replace")
                if "vg_canopy: " + want in buf:
                    print("  cockpit: %s" % want)
                    return
        sys.exit("the board never acknowledged '%s'. Either it is running a build\n"
                 "from before the cockpit switch existed -- see vg_capture.cpp -- or\n"
                 "the port is not the board. Not measuring: a run that cannot state\n"
                 "which cockpit it flew has nothing to compare."
                 % ("O" if delta else "o"))
    finally:
        ser.close()


def choose_warp(port, warp):
    """Tell the board to hold the cockpit's bend, and refuse to go on unless it says so.

    Same shape as choose_canopy, for the same reason: a run that cannot state how
    much the frame was bent has nothing to compare. Not sent at all when no hold is
    asked for, because run() resets the board first and a fresh board follows the
    throttle.
    """
    letter, want = {"flat": (b"f", "FLAT"), "full": (b"F", "FULL"),
                    "rigid": (b"i", "RIGID")}[warp]
    ser = open_quiet(port)
    try:
        time.sleep(0.5)
        ser.reset_input_buffer()
        for _ in range(4):
            ser.write(letter)
            ser.flush()
            t0, buf = time.time(), ""
            while time.time() - t0 < 1.5:
                buf += ser.read(4096).decode("ascii", "replace")
                if "vg_canopy_warp: " + want in buf:
                    print("  bend: %s" % want)
                    return
        sys.exit("the board never acknowledged '%s'. It is running a build from\n"
                 "before the bend hold existed -- see vg_capture.cpp -- or the port\n"
                 "is not the board. Not measuring." % letter.decode())
    finally:
        ser.close()


def choose_hash(port, want):
    """Arm or disarm the device's picture hash, and refuse to go on unless it says so.

    'w' TOGGLES, and the setting survives a reset (it lives in RTC memory), so a run
    that does not ask for the hash has to make sure the last run did not leave it
    armed: a hashed run's timings are polluted by the fold. So this always runs,
    sends 'w' once to learn the state from the answer, and once more if the answer
    is not the one wanted.
    """
    ser = open_quiet(port)
    try:
        time.sleep(0.5)
        for attempt in range(3):
            ser.reset_input_buffer()
            ser.write(b"w")
            ser.flush()
            t0, buf = time.time(), ""
            while time.time() - t0 < 1.5:
                buf += ser.read(4096).decode("ascii", "replace")
                if "band hash ARMED" in buf or "band hash off" in buf:
                    break
            armed = "band hash ARMED" in buf
            if ("band hash ARMED" in buf or "band hash off" in buf) and armed == want:
                if want:
                    print("  picture hash: ARMED (timings polluted; compare the hash only)")
                return
        sys.exit("the board never settled the picture hash to %s. Not measuring."
                 % ("armed" if want else "off"))
    finally:
        ser.close()


def choose_resident(port):
    """Tell the board to read the opaque bake from PSRAM, and refuse to go on unless
    it says so. See `resident` in vg_canopy_op.cpp. Same shape as choose_canopy."""
    ser = open_quiet(port)
    try:
        time.sleep(0.5)
        ser.reset_input_buffer()
        for _ in range(4):
            ser.write(b"r")
            ser.flush()
            t0, buf = time.time(), ""
            while time.time() - t0 < 1.5:
                buf += ser.read(4096).decode("ascii", "replace")
                if "vg_canopy_op: RESIDENT" in buf:
                    print("  bake: PSRAM")
                    return
        sys.exit("the board never acknowledged 'r'. It is running a build from\n"
                 "before the residency experiment -- see vg_capture.cpp -- or the\n"
                 "port is not the board. Not measuring.")
    finally:
        ser.close()


def run(port, path, limit, delta_canopy=False, warp=None, resident=False, hash_on=False):
    ses = Session.load(path)
    n = len(ses.frames)
    if limit and limit < n:
        n = limit
    print("%s: %d frames, timed replay (no pixels)" % (os.path.basename(path), n))

    reset_board(port, settle=6.0)
    # WHICH COCKPIT, before the link opens and before a frame is sent. Stated
    # rather than toggled, so a run measures the one it asked for and not the one
    # the last run left -- and checked, for the reason choose_canopy gives.
    choose_canopy(port, delta_canopy)
    if warp:
        choose_warp(port, warp)
    if resident:
        choose_resident(port)
    choose_hash(port, hash_on)
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
    # Both in units of the 1.0 s read timeout below.
    FORGIVE_AFTER = 3     # a partial batch this quiet has lost the rest; carry on
    GIVE_UP       = 40    # ...but total silence past the device's own 30 s is the end
    lost = [0]
    link.ser.timeout = 1.0
    started = time.time()
    sent = shown = 0

    while sent < n:
        k = min(CHUNK, n - sent)
        for i in range(k):
            link.replay_send(ses.frames[sent + i])
        sent += k

        # One byte back per frame drawn. A short read is the device still working, not an
        # error -- only a long silence is.
        #
        # A LOST ACK MUST NOT END THE RUN, and it used to. The ack is FLOW CONTROL, not
        # data: its only job is to stop the host putting more than the device's 2048 byte
        # ring can hold. The frame it refers to has already been drawn and its cost
        # counted on the device. So one byte lost to a corrupted link cost nothing real,
        # and the old loop waited for it for ever -- host blocked reading, device blocked
        # waiting for the next record, and the device's 30 s timeout fired ten seconds
        # after this loop had already given up and exited.
        #
        # Measured, not guessed: the device reported END 768 frames while this counted
        # 767. Exactly one byte, on a link that corrupts a band or three every run.
        #
        # So a quiet window now FORGIVES the missing acks and carries on. The accounting
        # drifts by however many were lost, which costs at most a few extra records
        # outstanding against a ring that holds 23.
        got = 0
        quiet = 0
        while got < k:
            b = link.ser.read(k - got)
            if b:
                got += len(b)
                quiet = 0
            else:
                quiet += 1
                if quiet == FORGIVE_AFTER and got > 0:
                    lost[0] += k - got
                    break
                # Nothing at all for a long time. The device gives up at 30 s, so this
                # must be LONGER than that or its explanation is generated after this
                # loop has stopped listening -- which is what made three runs in a row
                # report "it said nothing at all" when it had said exactly why.
                if quiet > GIVE_UP:
                    # ASK IT WHY BEFORE GIVING UP. The device ends a replay for six distinct
                    # reasons and prints which -- see s_why in vg_replay.cpp -- but it prints
                    # that when the replay ENDS, which is exactly the moment this loop used
                    # to exit and stop reading. So the one line explaining the failure was
                    # generated and thrown away every time, and three runs in a row were
                    # diagnosed as "it reset, or it desynced" when the device had said.
                    tail = bytearray()
                    end = time.time() + 6.0
                    while time.time() < end:
                        w = link.ser.in_waiting
                        if w:
                            tail += link.ser.read(w)
                        else:
                            time.sleep(0.02)
                    said = [l.strip() for l in
                            tail.decode("utf-8", "replace").splitlines()
                            if "vg_replay:" in l]
                    sys.exit("the device stopped answering after %d of %d frames.\n  %s"
                             % (sent - k + got, n,
                                "\n  ".join(said) if said else
                                "It said nothing at all, so it reset rather than ended."))
        if sent - shown >= 1000:
            shown = sent
            rate = sent / max(time.time() - started, 1e-6)
            print("  ...%d/%d  %.0f fps  %.0fs left"
                  % (sent, n, rate, (n - sent) / max(rate, 1e-6)))

    if lost[0]:
        print("  %d ack(s) lost to the link, forgiven -- the frames were drawn"
              % lost[0])
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
    # NOT SENT FROM HERE ANY MORE. This handle has just carried about eleven thousand
    # operations and the LAST WRITE on it is where this tool actually crashed -- proved by
    # a run whose log ends on the line above and never reaches the next stage.
    #
    # os._exit was tried first, on the theory that pyserial's teardown was the fault. It is
    # not: the process never got as far as teardown. The four bytes go from the fetch
    # process instead, which opens a fresh handle moments later and has carried nothing.

    # AND GO WITHOUT CLOSING IT. The handle has just carried about eleven thousand
    # operations, and pyserial's teardown on one that large is where this tool segfaulted --
    # after every frame was drawn, so the run was complete and the exit code said otherwise.
    #
    # fetch()'s docstring has said since it was written that "the interpreter dies on exit"
    # and that a fresh process is the only thing that reliably reads a device afterwards.
    # os._exit was the fix, and it was applied to the FETCH branch only -- the half that had
    # carried a few hundred operations rather than eleven thousand.
    #
    # So this half gets it too, and the port is released by the process ending rather than
    # by the serial layer unwinding. The OS closes the handle either way; only one of the
    # two routes crashes.
    sys.stdout.flush()
    os._exit(0)


def fetch(port, session=None, warp=None, resident=False):
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

    # END THE REPLAY FROM HERE, on a handle that has carried nothing.
    #
    # Any four bytes that are not "PHRP" end it; the device tests the mismatch, not the
    # content. A lone 'E' is one quarter of a record tag, so the device would sit out its
    # thirty second timeout before deciding the session was over.
    #
    # This must not reset the board -- a reset clears the sums this is here to collect --
    # and it does not, because the run process closed the port moments ago. That timing is
    # the same one fetch() has always depended on.
    ser.write(b"EEEE")
    ser.flush()
    time.sleep(0.5)

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
        # BLIT prints after WORLD, so it is the one to wait for -- returning on WORLD
        # would drop the blit split exactly as returning on COST once dropped the world.
        # And TYPES prints last of all, after the dips, so it is the one to wait for now:
        # returning on BLIT read the dips only when the rest of the report happened to
        # be in the same read.
        if m and (TYPES.search(txt) or time.time() > deadline - 8.0):
            ser.close()
            g = [int(x) for x in m.groups()]
            out = {"frames": g[0], "commit": git_commit(), "session": session or "(unknown)",
                   "warp": warp or "throttle", "bake": "psram" if resident else "flash"}
            for j, k2 in enumerate(KEYS):
                out[k2] = {"mean": g[1 + j * 2], "worst": g[2 + j * 2]}
            w = WORLD.search(txt)
            if w:
                gw = [int(x) for x in w.groups()]
                for j, k2 in enumerate(WKEYS):
                    out[k2] = {"mean": gw[j * 2], "worst": gw[j * 2 + 1]}
            bd = BANDS.search(txt)
            if bd:
                out["band_window"] = int(bd.group(1))
                out["bands"] = [int(x) for x in bd.group(2).split()]
            sb = SUB.search(txt)
            if sb:
                gs = [int(x) for x in sb.groups()]
                for j, k2 in enumerate(SKEYS):
                    out[k2] = {"mean": gs[j * 2], "worst": gs[j * 2 + 1]}
            ty = TYPES.search(txt)
            if ty:
                gt = [int(x) for x in ty.groups()]
                for j, k2 in enumerate(TKEYS):
                    out[k2] = {"mean": gt[j * 2], "worst": gt[j * 2 + 1]}
            rows = []
            for sm in SLOW.finditer(txt):
                rows.append({k: (int(v) if v is not None else None) for k, v in zip(SLOWK, sm.groups())})
            if rows:
                out["slow"] = rows
            sl = SLOWEST.search(txt)
            if sl:
                out["slowest"], out["under60"], out["under50"] = [int(x) for x in sl.groups()]
            hs = HIST.search(txt)
            if hs:
                out["hist"] = [int(x) for x in hs.groups()]
            bh = BANDH.search(txt)
            if bh:
                out["bandh"] = bh.group(1)
                out["bandh_bands"] = int(bh.group(2))
                out["bandh_every"] = int(bh.group(3))
            cn = CAN.search(txt)
            if cn:
                out["can_c0"], out["can_c1"], out["can_at"] = [int(x) for x in cn.groups()]
            b = BLIT.search(txt)
            if b:
                gb = [int(x) for x in b.groups()]
                for j, k2 in enumerate(BKEYS):
                    out[k2] = {"mean": gb[j * 2], "worst": gb[j * 2 + 1]}
            return out
    ser.close()
    said = tail.decode("utf-8", "replace").strip()
    sys.exit("the device reported no cost line.\n  It said:\n    %s"
             % ("\n    ".join(said.splitlines()[-4:]) if said else "(nothing at all)"))


def show(r):
    print("\n%d frames, commit %s, bend %s, bake %s" % (r["frames"], r["commit"],
                                                        r.get("warp", "throttle"),
                                                        r.get("bake", "flash")))
    print("  %-6s %10s %10s" % ("", "mean us", "worst us"))
    for k in KEYS:
        print("  %-6s %10d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if all(k in r for k in WKEYS):
        print("  -- inside `world`; the WORST column is the one that matters --")
        for k in WKEYS:
            print("  %-6s %10d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if all(k in r for k in BKEYS):
        # `blit` is join + wait + rast + push + res by construction. Only two of those
        # are interesting here: `push` is the CPU stopped against a full queue, which is
        # idle time under a busy wire, and `over_us` is the raster that outran its band
        # window, which is the only part of the raster that reaches the frame.
        print("  -- inside `blit`; push is idle CPU, over_us is frame time lost --")
        for k in BKEYS:
            print("  %-6s %10d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if all(k in r for k in SKEYS):
        print("  -- inside `sub`; wait is core 1 idle at the rendezvous --")
        for k in SKEYS:
            print("  %-9s %7d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if all(k in r for k in TKEYS):
        print("  -- primitive raster by type; aa is the blended lines (trails) --")
        for k in TKEYS:
            print("  %-6s %8d %10d" % (k, r[k]["mean"], r[k]["worst"]))
    if r.get("bands"):
        w = r.get("band_window", 768)
        over = [(i, v) for i, v in enumerate(r["bands"]) if v > w]
        print("  -- each band against its %d us window; a band under it is free --" % w)
        print("     " + " ".join(("%d*" % v) if v > w else str(v) for v in r["bands"]))
        if over:
            print("     %d of %d over, by %d us a frame (* marks them)"
                  % (len(over), len(r["bands"]), sum(v - w for _, v in over)))
    if "under60" in r:
        print("  -- the dips; frame time is upd + sub + max(rast, wire) --")
        print("  slowest %d us | %d frames under 60 fps, %d under 50" % (r["slowest"], r["under60"], r["under50"]))
        if "hist" in r:
            print("  60+ %d | 57-60 %d | 54-57 %d | 50-54 %d | under 50 %d" % tuple(r["hist"]))
    if "bandh" in r:
        print("  picture hash %s over %d bands (1 frame in %d) -- timings above are polluted"
              % (r["bandh"], r["bandh_bands"], r["bandh_every"]))
    if r.get("slow"):
        print("  -- the slowest frames, and what each was made of --")
        print("  %6s %7s %5s %6s %6s %6s | %5s %5s %5s %5s %5s"
              % ("frame", "us", "fps", "upd", "sub", "rast", "A", "B", "wait", "sxr", "world"))
        for s in r["slow"]:
            print("  %6d %7d %5d %6d %6d %6d | %5s %5s %5s %5s %5s"
                  % (s["frame"], s["us"], s["fps"], s["upd"], s["sub"], s["rast"],
                     *[("%d" % s[k]) if s[k] is not None else "-" for k in ("A", "B", "wait", "sxr", "world")]))
    if "can_c0" in r:
        print("  -- the canopy by core; `can` above is the slower half a band, summed --")
        print("  core0 %8d   core1 %8d   cut at row %d of 32" % (r["can_c0"], r["can_c1"], r["can_at"]))
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
    # A baseline saved before 2026-08-20 carries "(fetched)" here, because the name was
    # hardcoded and this guard could never fire. Those cannot be checked, so they are not
    # accused -- but they are the ones most likely to be the wrong session.
    old_s, new_s = was.get("session"), now.get("session")
    if old_s == "(fetched)":
        print("\n  BASELINE PREDATES SESSION STAMPING. It does not record which recording it\n"
              "  was measured over, so nothing here can confirm both runs saw the same frames.\n"
              "  Take a fresh baseline before trusting a small change.")
    elif old_s != new_s:
        print("\n  DIFFERENT SESSIONS (%s vs %s). The whole point of this tool is that\n"
              "  both runs see the same frames, so this comparison means nothing."
              % (old_s, new_s))
    if was["frames"] != now["frames"]:
        print("\n  Different frame counts (%d vs %d): one run ended early, so the means\n"
              "  are over different work." % (was["frames"], now["frames"]))
    if "bandh" in was and "bandh" in now:
        if was["bandh"] == now["bandh"] and was["bandh_bands"] == now["bandh_bands"]:
            print("\n  PICTURE IDENTICAL: hash %s over %d bands in both runs."
                  % (now["bandh"], now["bandh_bands"]))
        else:
            print("\n  PICTURE DIFFERS: hash %s over %d bands, was %s over %d. Not the same pixels."
                  % (now["bandh"], now["bandh_bands"], was["bandh"], was["bandh_bands"]))
    if "under60" in was and "under60" in now:
        print("  %-9s %10d %12d %9d" % ("under60", was["under60"], now["under60"], now["under60"] - was["under60"]))
        print("  %-9s %10d %12d %9d" % ("under50", was["under50"], now["under50"], now["under50"] - was["under50"]))
    old_w, new_w = was.get("warp", "throttle"), now.get("warp", "throttle")
    if old_w != new_w:
        print("\n  DIFFERENT BEND (%s vs %s). This is a measurement of the stretch: `can`\n"
              "  is the only figure that should move." % (old_w, new_w))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--delta-canopy", action="store_true",
                    help="fly the light-delta cockpit instead of the opaque "
                         "bake. Run once with and once without to compare "
                         "them on ONE board and one session.")
    ap.add_argument("--warp", choices=["flat", "full", "rigid"],
                    help="hold the cockpit's throttle bend at none or at full for "
                         "the whole run, instead of following the recorded throttle. "
                         "Run once each way to measure what the stretch costs. "
                         "'rigid' also holds the lag off: the frame does not move.")
    ap.add_argument("--resident", action="store_true",
                    help="read the opaque bake from a copy in PSRAM instead of "
                         "from flash. An experiment; see vg_canopy_op.cpp.")
    ap.add_argument("--hash", action="store_true",
                    help="fold every Nth frame's pixels into a hash on the device and "
                         "record it. Two builds that draw the same picture give the same "
                         "hash. The run's timings are polluted by the fold.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--frames", type=int, default=0,
                    help="stop after this many frames (default: the whole session)")
    ap.add_argument("--save", metavar="FILE", help="write the result")
    ap.add_argument("--against", metavar="FILE", help="compare with a saved result")
    ap.add_argument("--run-only", action="store_true",
                    help=argparse.SUPPRESS)   # internal: the half that drives the session
    ap.add_argument("--fetch", action="store_true",
                    help="do not run; just read the last run's result off the device")
    a = ap.parse_args()

    if a.fetch:
        r = fetch(a.port, os.path.basename(a.session), a.warp, a.resident)
    elif a.run_only:
        run(a.port, a.session, a.frames, a.delta_canopy, a.warp, a.resident, a.hash)   # never returns
        return
    else:
        # THREE PROCESSES, AND EACH ONE EXISTS FOR A CRASH.
        #
        # This one never opens the port at all. It starts the run, waits for it, then asks
        # for the answer -- so the only process that touches a handle carrying a full
        # session is one whose whole job is to die immediately afterwards.
        #
        # It was two processes, and the parent did the run itself. That parent then had to
        # unwind pyserial on an exhausted handle, which is a segfault about one run in four:
        # every frame drawn, the report never printed, exit code 139.
        # -u ON THE CHILD, and it is not a nicety.
        #
        # Its stdout is a pipe, so Python block-buffers it: the progress lines sat in the
        # child's buffer for the whole five minutes and this process printed NOTHING until
        # the child exited. A run that emits nothing for five minutes looks dead to
        # anything supervising it, and gets reaped -- which showed up as rc=127 with an
        # empty log, only ever on FULL runs in the background. A 400-frame run finished
        # before it mattered; the two-process design printed progress from here and never
        # hit it at all.
        argv = [sys.executable, "-u", os.path.abspath(__file__),
                a.session, "--port", a.port, "--run-only"]
        if a.frames:
            argv += ["--frames", str(a.frames)]
        # AND WHICH COCKPIT. Forgetting this is not a small bug: the child chose the
        # default either way, so --delta-canopy produced a run identical to the one it
        # was compared against, and the tool reported "+0.0% change" with total
        # confidence. Every flag `run` reads has to be forwarded here.
        if a.delta_canopy:
            argv += ["--delta-canopy"]
        if a.warp:
            argv += ["--warp", a.warp]
        if a.resident:
            argv += ["--resident"]
        if a.hash:
            argv += ["--hash"]
        # PIPED AND RE-EMITTED, not inherited. Inheriting this process's stdout is the
        # obvious way to keep the progress line live, and it works from a terminal and
        # fails when something else owns the handle -- a background runner, a redirect
        # set up by a harness -- where the child cannot inherit it and dies before it
        # starts. That showed up as five identical runs exiting 127 with empty logs,
        # which reads exactly like "python not found" and is not that at all.
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, bufsize=1)
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
        rc = proc.wait()
        if rc != 0:
            sys.exit(rc)

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
        # The bend and the bake go to the fetch as well, so the saved result records them.
        if a.warp:
            argv += ["--warp", a.warp]
        if a.resident:
            argv += ["--resident"]
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
