#!/usr/bin/env python3
"""Fire the game's explosions on the device, to look at them.

    python tools/phantom_vfx.py --port COM6            # one shot, next preset
    python tools/phantom_vfx.py --port COM6 --auto     # start or stop the repeat
    python tools/phantom_vfx.py --port COM6 --loop 1.2 # fire from the host

Leave the game on the title screen. The effects run there, so you do not need
to play a match to see them.

There are four presets and a shot steps to the next one:

    0  missile fuse expires
    1  missile hit
    2  ship destroyed
    3  player wreck

The device prints the name of each shot. This program shows what the device
says.

The device refuses these commands during a record or a render. The effects draw
from the seeded random number generator, so a shot in the middle of a render
would move the simulation off the sequence the recording was made from.
"""

import argparse
import sys
import time

import serial


def open_port(port):
    """Open the port WITHOUT resetting the device.

    A normal open asserts DTR and RTS, and on this board those lines are the
    reset. The device would restart at every command, which loses the repeat
    setting and restarts the game. Both lines must be cleared before the open,
    not after: after is already too late.
    """
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.4
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def drain(ser, secs):
    """Show what the device says for `secs` seconds."""
    end = time.time() + secs
    while time.time() < end:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", "replace").strip()
            if text:
                print(f"  {text}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--auto", action="store_true",
                    help="turn the device's own repeat on or off. It fires every "
                         "1.6 s and needs no host after that.")
    ap.add_argument("--loop", type=float, metavar="SECONDS",
                    help="fire from the host at this interval. Press Ctrl+C to "
                         "stop. Use this to set an interval the device does not "
                         "offer.")
    ap.add_argument("--count", type=int, default=1,
                    help="number of shots for a plain run. The default is 1.")
    args = ap.parse_args()

    ser = open_port(args.port)
    try:
        if args.auto:
            ser.write(b"X")
            ser.flush()
            drain(ser, 1.0)
            return

        if args.loop:
            print("Firing. Press Ctrl+C to stop.")
            try:
                while True:
                    ser.write(b"x")
                    ser.flush()
                    drain(ser, args.loop)
            except KeyboardInterrupt:
                print("\nStopped.")
            return

        for _ in range(max(1, args.count)):
            ser.write(b"x")
            ser.flush()
            drain(ser, 0.6)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
