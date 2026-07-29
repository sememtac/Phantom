# Capture tools

Recording Phantom off the board at a true 60 fps, instead of pointing a phone
at it.

## Why it is two steps

A frame is 480×480×2 = 460,800 bytes and the link carries 0.74 MB/s. Run-length
coded, a typical frame is about 33 KB — roughly **23 frames a second**, not
sixty. And that is the ceiling for *moving pixels*, not for playing: sending
them while you play stalls the game loop to **15 fps**, which does not record
this game so much as a slower, worse one.

So nothing is sent while you play.

**Record** logs the *simulation* instead of the picture — a frame duration and
an input struct, 71 bytes a frame, about 4 KB/s. The game runs at its true,
unimpeded speed. Measured: 240 frames of play in 3.57 s of wall clock for
3.73 s of gameplay — 0.96× realtime, 64 fps, 17 KB.

**Render** re-runs that session on the device afterwards, frame by frame, and
pulls the real pixels back as slowly as it likes. Costs about **3.3 minutes per
minute** of gameplay.

The video is a genuine 60 fps because the frames really were 1/60 s apart when
they happened, and every pixel is the actual rasteriser output — the HUD's own
fps counter reads whatever it read at the time. Every frame arrives whole, with
nothing dropped and no tearing, none of which is true of filming the panel.

Recording **restarts the game**, because a session has to begin somewhere the
replay can also begin. Play from the menu.

## PhantomRecorder (the app)

Pick the port and an output folder, then work down the window: **1. Record
Gameplay**, play, **Stop**, then **2. Render to Video**. It writes a
timestamped `.phr` session and then an `.mp4` beside it, and shows what it is
pulling while it renders.

`File → Open Session...` renders a `.phr` from an earlier sitting.

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

## phantom_session.py (command line)

Same thing without the window, for scripting:

```
python tools/phantom_session.py record --port COM6 --out run.phr   # play, Ctrl+C
python tools/phantom_session.py render --port COM6 run.phr --dir .
```

## How replay can be exact

The simulation is a pure function of (seed, dt, input): the game draws from a
seeded xorshift, and no game or render code reads the wall clock. The four
`esp_random()` calls that do exist go through `vg_replay_rand()`, which logs
them while recording and hands the same values back while replaying. Touch and
the IMU are never read during playback — whatever they produced is already in
the recorded input struct.

Persisted progress — credits, callsign, ship, champion, trail hue — is
snapshotted into the session header and restored on playback, and saving to
flash is suppressed while replaying, so rendering a recording cannot overwrite
the progress of whoever made it.

## Things that were tried and are not here

**Live capture**, which streamed frames while you played. Real time, and it
dropped the game to 15 fps. The whole point of the project is 60, so it is gone.

**Smooth capture**, which stepped the simulation on a fixed clock so the video
came back perfectly smooth. But that is 60 fps video of a board running in slow
motion, and the session workflow gives the same result from a game that really
ran at 60.

**Band delta compression.** Sending each band as its XOR against the previous
frame cut a frame from 33 KB to **13.0 KB** — 2.5×, with 3499 of 3600 bands
choosing the delta. It still made rendering **slower**: 19.5 fps → 16.3, and
17.3 after cutting it to a single pass. Holding the previous frame in PSRAM
costs about 900 KB of traffic per frame to read and write, and PSRAM is slow
enough that this outweighs the ~27 ms of link time the smaller frames saved —
especially since the USB driver drains in the background while the CPU works,
so link time was already partly free. Rendering is device-bound, not
link-bound. Reverted, and worth not rediscovering.

The remaining lever for the render pass is overlapping the device's rasterising
with its transmitting on the second core, which is untried.

## Why the recording looks darker than the panel

The capture is a faithful copy of the framebuffer: a 5-bit 0x1F really does come
out as 255. What it cannot copy is the panel, which is an emissive AMOLED with
true blacks driven hard, and which reads considerably punchier than the same
numbers on a monitor. No amount of correctness in the conversion closes that.

Two things do help. The mp4 is now tagged bt709 / limited range explicitly, in
both the container and the stream's own VUI -- untagged, players guess, and one
that guesses wrong on a picture this dark crushes the blacks. And `--gamma`
lifts the midtones towards how the panel reads:

```
python tools/phantom_session.py render --port COM6 run.phr --gamma 1.5
```

It is a matching control, not a correction: 1.0 is the faithful copy and the
default.

The lift is applied to each pixel's **value** � its largest channel � with all
three channels scaled by that same factor, so hue and saturation come through
untouched. Applying a curve per channel instead, which is the obvious way, is
wrong for this game: the HUD amber is `#ffae18`, red already pinned at 255, so
only green and blue can move and the colour rotates towards yellow while going
pale. Measured 39� / 91% saturation at gamma 1.0 against 43� / 79% at 1.5 � an
interface built on one amber shows that first. Hue now holds at 32.3� across
1.0 to 1.8 while a dim amber lifts from 38% to 53% brightness.

A pixel already at 255 in some channel cannot get brighter, which is correct �
it is already as bright as the format goes. What lifts is everything below it.

## Those vertical stripes are the scanline effect

They are not a recording artifact and not your monitor. `band_scanlines` in
`vg_band.cpp` darkens every other **panel row**, and with `VG_ROTATE 1` the
panel is mounted a quarter turn off -- so a panel row lands as a constant *x*
in the upright picture. The CRT scanline effect has always run vertically, on
the device too. At ~313 PPI on a 2.16-inch panel it is nearly invisible;
magnified on a monitor it is obvious.

Turning it back to horizontal means darkening every other panel COLUMN, which
means touching every row instead of every second one. The pass currently
measures ~3.3 ms a frame, so expect roughly double -- about 3 ms out of a 16.6 ms
budget, against a game that already dips to 52 fps. Not done, deliberately.

## Layout

| file | |
|---|---|
| `phantom_link.py` | wire protocol, session format, pixel conversion — the only copy |
| `phantom_recorder.py` | the window |
| `phantom_session.py` | the command line |

Both front ends drive `phantom_link`, deliberately. The two bugs that took the
first version two attempts — scanning for the frame magic inside binary
payloads, and copying the firmware's rotation instead of inverting it — are
exactly the kind that get fixed in one copy and left in the other.

## Requirements

`pyserial`, and `numpy` for any useful speed — without it the pixel conversion
runs a Python loop over 230,400 pixels per frame and rendering crawls. `ffmpeg`
on PATH for mp4 output; without it a PPM sequence is written instead, which
ffmpeg or almost anything else can convert later. The exe bundles all three.
