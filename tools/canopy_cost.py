#!/usr/bin/env python3
"""Ask the board what the canopy costs to draw.

    python tools/canopy_cost.py COM6

The board draws the canopy only inside a match, and the frame counter reports one
band's slower half rather than the whole pass. This runs the pass on one core and
reports it flat, so a new drawing can be judged from the menu.

The number is the drawing time on one core. The frame pays about two thirds of it,
because the two cores share the pass and one of them finishes late.

Close every other program that uses the port first.
"""
import sys
import time

import serial

REPEATS = 5


def main(port):
    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(2.0)                    # the board prints telemetry; let it settle
    ser.reset_input_buffer()

    got = []
    for _ in range(REPEATS):
        ser.write(b"k")
        ser.flush()
        deadline = time.time() + 4.0
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line.startswith("canopy:"):
                print("  " + line)
                got.append(int(line.split()[1]))
                break
    ser.close()

    if not got:
        sys.exit("no answer from the board. Is another program holding the port?")

    # The first reading is high every time: the table and the code are read through
    # the flash cache, and the first pass fills it. Later passes are the real cost.
    steady = got[1:] or got
    us = sum(steady) / float(len(steady))
    print(f"\n{us:.0f} us of drawing on one core")
    print(f"the frame pays about {us * 0.67 / 1000.0:.2f} ms of it")
    if us * 0.67 > 2500.0:
        print("TOO HEAVY: at this cost the game drops to about 50 frames a second.")
        print("   Narrowing the shapes is the lever that matters.")
    elif us * 0.67 > 1200.0:
        print("HEAVY: expect to lose 60 in busy fights.")
    else:
        print("there is room for it")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1])
