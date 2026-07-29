# Capture tools

Recording Phantom off the board, instead of pointing a phone at it.

## PhantomRecorder (the app)

A small window: pick the port and an output folder, set a length, press
**Record**. It shows a live preview of what it is pulling and writes a
timestamped `phantom-YYYYMMDD-HHMMSS.mp4` into the folder you chose.

Pressing **Stop** keeps whatever has already arrived — a recording cut short is
still a recording.

Run `tools/dist/PhantomRecorder.exe`, or from source:

```
python tools/phantom_recorder.py
```

To rebuild the executable:

```
cd tools
python -m PyInstaller --noconfirm --onefile --windowed ^
    --name PhantomRecorder --add-data "phantom_link.py;." phantom_recorder.py
```

## phantom_capture.py (command line)

Same thing without the window, for scripting:

```
python tools/phantom_capture.py --port COM6 --seconds 12 --out demo.mp4
```

## Why it is slow, and why that does not matter

A frame is 480×480×2 = 460,800 bytes and the USB CDC link carries roughly a
megabyte a second. Two frames per second raw, about five with the run-length
coding the firmware applies. There is no compression scheme that turns that
into sixty.

So capture does not run in real time and does not try to. While recording is
armed the firmware steps its simulation at a **fixed 30 fps** no matter how long
each frame actually took. The board runs in slow motion; the recording plays
back perfectly smooth. Wall-clock speed decides how long you wait, not how the
video looks — and every frame arrives whole, with nothing dropped and no
tearing, none of which is true of filming the panel.

Expect roughly **five seconds of waiting per second of footage**.

The consequence worth knowing: a match cannot really be *played* at five frames
a second. The attract loop, the menus, the bracket and the launch cutscene all
record perfectly hands-off. Live combat is best taken in short bursts.

## Layout

| file | |
|---|---|
| `phantom_link.py` | wire protocol and pixel conversion — the only copy |
| `phantom_recorder.py` | the window |
| `phantom_capture.py` | the command line |

Both front ends drive `phantom_link`, deliberately. The two bugs that took the
first version two attempts — scanning for the frame magic inside binary
payloads, and copying the firmware's rotation instead of inverting it — are
exactly the kind that get fixed in one copy and left in the other.

## Requirements

`pyserial` for either script (the exe bundles it). `ffmpeg` on PATH for mp4
output; without it both fall back to writing a PPM sequence, which ffmpeg or
almost anything else can convert later.
