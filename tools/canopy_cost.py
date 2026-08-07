#!/usr/bin/env python3
"""Ask the board what the canopy costs to draw.

    python tools/canopy_cost.py COM7

Run it from any state, the title screen included. The board picks a drawing to
measure: the hull in the saved profile, or any hull that has one. It names the hull
it measured.

The number is the drawing time on one core. The frame pays about three fifths of it,
because the two cores share the pass and one of them finishes late.

The tool refuses to grade a reading of zero. A drawing that covers no pixels is not a
cheap drawing, it is a missing measurement.

Close every other program that uses the port first.
"""
import re
import sys
import time

import serial

from phantom_link import open_quiet

REPEATS = 5

# The device prints one line for each bench. `blocks` is the honest test for a real
# reading: it is the count of runs in the table, so it cannot be zero for a drawing
# that exists.
LINE = re.compile(r"^canopy:\s+(\d+)\s+us rigid.*?(\d+)\s+blocks"
                  r"(?:.*?hull\s+(\S+))?", re.I)


def main(port):
    # open_quiet, NOT serial.Serial: the plain constructor restarts the board on
    # connect, so asking what the canopy costs would first throw the player out of
    # the match they were measuring. See phantom_link.open_quiet.
    ser = open_quiet(port, timeout=0.5)
    time.sleep(2.0)                    # the board prints telemetry; let it settle
    ser.reset_input_buffer()

    got, blocks, hull = [], 0, None
    for _ in range(REPEATS):
        ser.write(b"k")
        ser.flush()
        deadline = time.time() + 4.0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            m = LINE.match(line)
            if m:
                print("  " + line)
                got.append(int(m.group(1)))
                blocks = int(m.group(2))
                hull = m.group(3)
                break
    ser.close()

    if not got:
        sys.exit("no answer from the board. Is another program holding the port?")

    # NEVER GRADE A ZERO. This is the whole reason the file changed.
    #
    # The bench returns a zeroed result when the board has no drawing selected, and the
    # old code took that at face value: 0 us passed every threshold, so it printed
    # "there is room for it". It said that three times over three flashes of a drawing
    # the baker had already called TOO HEAVY.
    #
    # `blocks` is the test rather than the microseconds. A real drawing has thousands of
    # runs in its table, and a pass over it cannot take zero time -- but a fast pass and
    # an absent one both round to 0 us, so the block count is what tells them apart.
    if blocks == 0 or max(got) == 0:
        sys.exit(
            "the board measured no drawing, so there is nothing to report.\n"
            "  It said %d blocks and hull %s.\n"
            "  A hull with no canopy reads this way, and so does a build where\n"
            "  design/canopy/ produced nothing. Check that the hull you are\n"
            "  measuring has a PNG, then run tools/canopy_set.py again."
            % (blocks, hull or "unknown"))

    # The first reading is high every time: the table and the code are read through
    # the flash cache, and the first pass fills it. Later passes are the real cost.
    steady = got[1:] or got
    us = sum(steady) / float(len(steady))
    frame_ms = us * 0.59 / 1000.0

    print("\n%s: %.0f us of drawing on one core, %d blocks"
          % (hull or "canopy", us, blocks))
    print("the frame pays about %.2f ms of it" % frame_ms)

    # The thresholds are the baker's, so one drawing gets the same verdict from the
    # model and from the board. See tools/canopy_bake.py.
    if frame_ms > 2.5:
        print("TOO HEAVY: at this cost the game drops to about 50 frames a second.")
        print("   Narrowing the shapes is the lever that matters.")
    elif frame_ms > 1.2:
        print("HEAVY: expect to lose 60 in busy fights. Narrowing the shapes is the")
        print("   only lever that matters -- levels and detail are free, area is not.")
    else:
        print("there is room for it")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1])
