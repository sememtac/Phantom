#!/usr/bin/env python3
"""Ask the board what each canopy costs to draw.

    python tools/canopy_cost.py COM7

Run it from any state, the title screen included. The board measures every hull
that has a drawing and prints one line for each.

The numbers are the time of the whole cockpit pass on one core, in microseconds:
once with the frame rigid, and once at full bend. In flight the two cores share the
pass, so the frame pays less than the number shown. The share is not measured for
this pass. To learn what a drawing costs the frame, use `tools/replay_cost.py`.

Close every other program that uses the port first.
"""
import re
import sys
import time

import serial

from phantom_link import open_quiet

REPEATS = 4

# The device prints one line for each hull that has a drawing.
LINE = re.compile(r"^canopy_op:\s+(\S+)\s+(\d+)\s+us rigid,\s+(\d+)\s+us full bend,"
                  r"\s+(\d+)\s+px", re.I)


def main(port):
    # open_quiet, NOT serial.Serial: the plain constructor restarts the board on
    # connect, so a question about the canopy would first throw the player out of
    # the match they were in. See phantom_link.open_quiet.
    ser = open_quiet(port, timeout=0.5)
    time.sleep(2.0)                    # the board prints telemetry; let it settle
    ser.reset_input_buffer()

    runs = {}                          # hull -> list of (rigid, full, px)
    for _ in range(REPEATS):
        ser.write(b"k")
        ser.flush()
        deadline = time.time() + 6.0
        seen = 0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            m = LINE.match(line)
            if not m:
                continue
            hull = m.group(1)
            runs.setdefault(hull, []).append(
                (int(m.group(2)), int(m.group(3)), int(m.group(4))))
            seen += 1
            if seen >= 4:
                break
    ser.close()

    if not runs:
        sys.exit("no answer from the board. Is another program holding the port?\n"
                 "  A build with no drawing in design/canopy/ also answers nothing.")

    # The first reading is high every time: the table and the code are read through
    # the cache, and the first pass fills it. Later passes are the real cost.
    print("\n%-9s %10s %14s %9s" % ("hull", "rigid us", "full bend us", "pixels"))
    for hull, got in runs.items():
        steady = got[1:] or got
        rigid = sum(r for r, _, _ in steady) / float(len(steady))
        full = sum(f for _, f, _ in steady) / float(len(steady))
        px = steady[-1][2]
        print("%-9s %10.0f %14.0f %9d" % (hull, rigid, full, px))
    print("\nOne core, the whole pass. To see what the frame pays, run tools/replay_cost.py.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1])
