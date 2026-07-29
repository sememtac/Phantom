# Capture tools

Recording Phantom off the board, instead of pointing a phone at it.

## PhantomRecorder (the app)

A small window: pick the port and an output folder, set a length, press
**Record**. It shows a live preview of what it is pulling and writes a
timestamped `phantom-YYYYMMDD-HHMMSS.mp4` into the folder you chose.

Pressing **Stop** keeps whatever has already arrived — a recording cut short is
still a recording.

Tick **Continuous** to record until you press Stop — for a whole playthrough
rather than a clip. Frames stream to disk as they arrive, so length is limited
by disk rather than memory, and continuous recordings are written as fragmented
mp4 so an unclean end costs the last fragment instead of the file.

**Live** (on by default) records what is on the panel, when it was on the panel.
Untick it for **smooth**. See below — the difference is the whole story of these
tools.

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
python tools/phantom_capture.py --port COM6 --seconds 12 --dir .
python tools/phantom_capture.py --port COM6 --smooth --seconds 12 --dir .
python tools/phantom_capture.py --port COM6 --continuous --dir .   # until Ctrl+C
```

## Live or smooth — pick one, you cannot have both

A frame is 480×480×2 = 460,800 bytes and the USB CDC link carries roughly a
megabyte a second. Two frames per second raw, about **six** with the run-length
coding the firmware applies. There is no compression scheme that turns that into
sixty, so something has to give: either the video is real time and choppy, or it
is smooth and slowed. Both modes exist because both are the right answer
sometimes.

**Live** (the default) leaves the game's clock alone. What goes out is exactly
what was on the panel at that moment, so the recording is real time — at about
six frames a second, because every send stalls the loop while it goes. The host
measures the true arrival rate over the first sixteen frames and encodes at that,
so the video runs at the speed the game actually ran. **This is the mode for
recording yourself playing.**

**Smooth** (`--smooth`, or untick Live) has the firmware step its simulation at a
fixed 30 fps however long each frame really took. The board runs in slow motion;
the recording plays back perfectly smooth. Wall-clock speed decides how long you
wait, not how the video looks. Expect roughly **six seconds of waiting per second
of footage** — and a match cannot really be *played* at six frames a second, so
this is for the attract loop, the menus, the bracket and the launch cutscene,
which record hands-off and look immaculate.

Either way every frame arrives whole, with nothing dropped and no tearing, none
of which is true of filming the panel.

Measured on the bench: 6 s of live capture → 39 frames, 5.8 s of video at
6.7 fps. 2 s of smooth capture → 60 frames at 30 fps, 13 s of waiting.

One firmware consequence worth recording: live capture is why `main.cpp`
sub-steps long frames instead of clamping them. A 180 ms send used to be clamped
to 100 ms, which would have advanced the world at half wall-clock speed and made
"real time" a lie by a factor of two.

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
