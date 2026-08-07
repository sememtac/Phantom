# Capture tools

These tools record Phantom from the device at 60 fps. The result is better than
a video of the panel.

## Why there are two steps

One frame is 480 x 480 x 2 = 460,800 bytes. The link carries 0.74 MB/s.
Run-length coding makes a typical frame about 33 KB. The link can carry about
23 of these frames each second, not 60.

That limit causes a second problem. If the device sends pixels while you play,
the game loop slows to 15 fps. The video then shows a slower game than the game
you play. For this reason the device sends no pixels while you play.

**Record** saves the simulation, not the picture. Each frame needs one frame
time and one input structure, which is 71 bytes. This is about 4 KB/s, so the
game keeps its full speed. A measurement: 240 frames of play took 3.57 s of
clock time for 3.73 s of game time. That is 0.96 times real time, at 64 fps, in
17 KB.

**Render** runs the session again on the device, one frame at a time. The device
sends the true pixels at the speed of the link. This step takes about 2.9
minutes for each minute of play.

The video is 60 fps because the game made the frames 1/60 s apart. Every pixel
is the output of the rasteriser. The fps counter in the HUD shows the value it
showed at the time. Every frame arrives complete, and no frame tears. A video of
the panel gives none of these.

## PhantomRecorder (the window)

**A record restarts the game, and it has to.** The render step replays your inputs
on the device to make the pixels again, so it must begin at a state it can
reproduce. Mid-flight is not one: the ship, the opponent, the random-number cursor
and the venue are not in the session header, only your progress is. A session
recorded from where the game happens to be would render as something you never
flew.

So every session opens with the taps that got you into a match. To keep those out
of the video, render a range instead of the whole thing:

```
python tools/phantom_session.py render --port COM6 run.phr --from 300 --to 1200
```

The device still simulates the frames before the range -- the state at frame 300 is
the product of the 299 before it -- so this does not make the render quicker. It
only stops the menus reaching the file.

Do these steps in order:

1. Select the port and the output folder.
2. Press **1. Record Gameplay**.
3. Play the game.
4. Press **Stop**.
5. Set the **Gamma** slider.
6. Press **2. Render to Video**.

The program makes a folder for each recording, named for the time it started, and
writes every file of that recording into it:

```
<output folder>/20260804-213000/phantom-20260804-213000.phr
<output folder>/20260804-213000/phantom-20260804-213000.mp4
```

A recording is one thing. After a few of them, a flat folder cannot say which
video came from which session.

To render a session again with a different gamma, use **File > Open Session**. The
new video is written next to that session, in the folder it already lives in. The window shows each frame during the render
step.

The program remembers the output folder, the gamma, and the port. They are kept in
`.phantom_recorder.json` in your home folder, and written whenever one of them
changes, so a crash does not lose them. A board that comes back on a different COM
number falls back to the first port in the list.

Start `tools/dist/PhantomRecorder.exe`. To start the program from the source
instead:

```
python tools/phantom_recorder.py
```

To build the program again:

```
cd tools
python -m PyInstaller --noconfirm --onefile --windowed ^
    --name PhantomRecorder --add-data "phantom_link.py;." phantom_recorder.py
```

## phantom_session.py (command line)

This program does the same work without a window.

```
python tools/phantom_session.py record --port COM6 --out run.phr   # play, then press Ctrl+C
python tools/phantom_session.py render --port COM6 run.phr --dir .
```

## Why the render step gives the same picture

The simulation is a pure function of the seed, the frame time, and the input.
The game gets its random numbers from a seeded xorshift. No game code and no
render code reads the clock.

The firmware has four calls to `esp_random()`. Each call goes through
`vg_replay_rand()`. This function saves the value during a record, and returns
the same value during a render. The device does not read the touch panel or the
IMU during a render, because the saved input structure already holds their
values.

The device puts the progress of the player in the session header: the credits,
the callsign, the ship, the champion flag, and the trail hue. It restores these
values at the start of a render. It also blocks every write to flash during a
render. A render therefore cannot change the progress of the player who made
the session.

A flash write during a render does more than change data. The write disables the
instruction cache while it runs, and that is long enough to break the pixel
stream. The host then reads a colour index that is not in the table and stops.
Two paths write to flash, and both are now blocked while the link is busy: the
player's progress, and the diagnostic record of the worst frame.

## Methods that were tried and removed

**Live capture** sent frames while you played. The video was real time, but the
game slowed to 15 fps. The project needs 60 fps, so this method was removed.

**Smooth capture** stepped the simulation with a fixed frame time. The video was
smooth, but the device ran in slow motion. The two steps above give the same
video from a game that ran at 60 fps.

**Band delta compression** sent each band as an XOR against the previous frame.
It made a frame 13.0 KB instead of 33 KB, which is 2.5 times smaller. 3499 bands
of 3600 used the delta. The render step became slower: 19.5 fps changed to 16.3
fps. One pass instead of two gave 17.3 fps.

The previous frame must stay in PSRAM. To read it and to write it costs about
900 KB of PSRAM traffic for each frame. PSRAM is slow, and that cost is more
than the link time the smaller frames saved. This change was removed.

The reason is the cost of THIS method, not the idea of compression. Do not read
it as proof that smaller frames cannot help. The numbers say the opposite:

| quantity | value |
|---|---|
| link, with no render and no encode (`b` command) | 0.885 MB/s |
| bytes for each frame | 25.3 KB |
| link time for one frame | 28.6 ms |
| measured time for one frame | 45.0 ms |
| CPU time that the link does not hide | 16.4 ms |

The link is 64% of the time of a frame. The render step is therefore limited by
the link, and a cheaper encoding does help, if it costs little CPU. The delta
method failed because it needed PSRAM, not because the frames became smaller.

**A colour table for each band** was tried and is in use. A run was a count and
a colour, which is 3 bytes. It is now a count and an index, which is 2 bytes,
with one table for each band. Measured over 900 bands of play: the median band
holds 19 colours and the largest holds 87. No band went above 256, so the index
always fits, and the code keeps the 3-byte form for a band that does not fit.
A frame went from 36.7 KB to 25.3 KB, and the render step went from 19.5 fps to
22.2 fps. The output is the same: three frames of a render are identical to the
same frames before the change.

**Transmit from the second core** was tried and is NOT in use. The idea is
correct: 16.4 ms of each frame is CPU time that the link does not hide, and
core 0 could send the frame while core 1 draws the next one. Both attempts gave
a corrupt stream. The host read a band length of 1,667,340,360 bytes. Memory
barriers on the ring did not fix it, and neither did moving the one printf that
also wrote to the port. The cause is not established.

The code is gone. `vg_capture.cpp` now writes through `vg_link_write()`, which
sends from the game core and gives up after a set number of tries. To try the
second core again, write it again, and find the fault first. A capture that is
wrong now and then is worse than a capture that is slow.

## Why the video is darker than the panel

The video holds the same values as the framebuffer. A 5-bit value of 0x1F
becomes 255. The panel is different. It is an emissive AMOLED with true black,
and it runs at high brightness. The same values look brighter on the panel than
on a monitor. A correct conversion cannot remove this difference.

Two things help.

First, the mp4 file now declares bt709 and limited range. The declaration is in
the container and in the VUI of the video stream. Without the declaration, a
player must guess the range. A wrong guess makes the black areas darker, and
this picture is mostly black.

Second, the `--gamma` option makes the dark parts brighter:

```
python tools/phantom_session.py render --port COM6 run.phr --gamma 1.5
```

A gamma of 1.0 keeps the values of the framebuffer. This is the default.

The program finds the largest channel of each pixel and gets a gain from it. It
then multiplies all three channels by that same gain. The hue and the saturation
do not change.

A gain curve on each channel is the usual method, and it is wrong for this game.
The amber colour of the HUD is `#ffae18`. Red is already 255 and cannot
increase. Only green and blue increase, so the colour turns towards yellow and
loses saturation. Measurements on that colour: 39 degrees and 91% saturation at
gamma 1.0, and 43 degrees and 79% at gamma 1.5. The interface uses this one
amber colour almost everywhere, so this change is easy to see. The hue now stays
at 32.3 degrees from gamma 1.0 to gamma 1.8. A dark amber increases from 38% to
53% brightness.

A pixel with a channel at 255 cannot become brighter. This is correct, because
255 is the maximum of the format. The gain applies to all values below it.

## The vertical stripes are the scanline effect

The stripes are not an error of the video, and not an error of your monitor. The
function `band_scanlines` in `vg_band.cpp` makes every second **panel row**
darker. `VG_ROTATE` is 1, so the panel is mounted at 90 degrees. A panel row
therefore becomes a column of constant *x* in the upright picture. The scanline
effect always ran vertically, on the device also. The panel has about 313 pixels
per inch on a screen of 2.16 inches, so the stripes are difficult to see there.
A monitor makes them larger and easy to see.

To make the stripes horizontal, the function must darken every second panel
**column**. It must then read every row, not every second row. The pass costs
about 3.3 ms for each frame now, so expect about 6.6 ms. The frame budget is
16.6 ms, and the game already falls to 52 fps. This change is not done.

## phantom_vfx.py (look at the explosions)

The explosions last about half a second and they happen when a missile goes off
or a ship dies. To judge one, you had to play a match to that moment, and then
play another match after each change. This program fires them on the device
instead.

Leave the game on the title screen. The effects run there.

```
python tools/phantom_vfx.py --port COM6            # one shot, next preset
python tools/phantom_vfx.py --port COM6 --auto     # the device repeats, every 1.6 s
python tools/phantom_vfx.py --port COM6 --loop 1.2 # the host repeats, your interval
```

`--auto` turns the repeat on, and a second `--auto` turns it off. The device
needs no host after that, so you can unplug the cable and watch.

This program opens the port WITHOUT resetting the device. A normal open asserts
DTR and RTS, and on this board those lines are the reset. Any other terminal you
use must do the same, or each command restarts the game and loses the repeat
setting. The first version of this program had that fault, and the effects
appeared to do nothing.

There are four presets and each shot steps to the next one:

| preset | effect |
|---|---|
| 0 | missile fuse expires |
| 1 | missile hit |
| 2 | ship destroyed |
| 3 | player wreck |
| 4 | point blank, inside the fire |

Preset 4 goes off on the canopy. Use it to judge the airframe rattle, the
instrument jitter and the panel glitch. The other four are too far away to start
those, because they only run while the cockpit is inside the fireball.

The device prints the name of each shot.

The commands are `x` for one shot and `X` for the repeat. Any terminal on the
port can send them; this program is only a convenience.

The device refuses both during a record and during a render. The effects draw
from the seeded random number generator, so a shot in the middle of a render
would move the simulation off the sequence the recording was made from.

## Files

| file | contents |
|---|---|
| `phantom_link.py` | the wire protocol, the session format, and the pixel conversion |
| `phantom_recorder.py` | the window |
| `phantom_session.py` | the command line |
| `phantom_vfx.py` | fires the explosions, to look at them |
| `canopy_bake.py` | turns a canopy drawing into a table the firmware draws |
| `canopy_cost.py` | asks the board what the canopy costs to draw |
| `canopy.ps1` | bake, build and flash a new canopy in one command |

## Canopy drawings

The canopy is the cockpit frame. It replaces the crosshair. Each ship can have its
own drawing.

### What the file must be

Use a square PNG, 480 x 480 pixels or larger. Two channels carry the work.

| channel | holds |
|---|---|
| red | the frame |
| green | the activation regions |
| blue | not read |

The baker reads the two channels separately. Do not send a grey image. A grey image
puts the same values in both channels, so the frame becomes its own mask.

### Red: the frame

The most common red value is the background. That value means "leave this pixel
alone". A brighter value adds light. A darker value takes light away.

The firmware applies the frame to the finished picture. The frame lights what is
behind it. It does not paint over it.

### Green: the activation regions

The cockpit comes online one region at a time. A region is a shape in the green
channel. The grey value of the shape is the order. The lowest value comes on first.

Each region flashes white. The world behind it then appears. The frame inside it
lights up and cools to the value you drew.

Obey these three rules:

1. Give each region one flat value. A soft edge between two regions makes a third
   value, and that value becomes a region.
2. Use 8 regions or fewer. The baker stops and shows the values it found if there
   are more.
3. Cover the whole image. Every pixel needs a region, not only the pixels the frame
   covers. The world stays black until its region comes on.

### The coverage budget

Area is the only cost. About 95% of the cost is the number of pixels the frame
covers. Levels, gradients and fine detail are free.

| the frame covers | what to expect |
|---|---|
| less than 6% | cheap |
| 6% to 10% | there is room for it |
| 10% to 14% | the game loses 60 frames a second in a busy fight |
| more than 14% | the game drops to about 50 frames a second |

The reference drawing covers 10.5%. To make a drawing cheaper, make the shapes
narrower. Do not remove detail. Detail costs nothing.

### Bake it

The file name is the wiring. Name the PNG after the ship that flies it.

1. Put the PNG in `design\canopy\`. Use one of these names:
   `aegis.png`, `lance.png`, `chariot.png`, `ballista.png`.
2. Run `.\tools\canopy.ps1`. It takes no arguments.
3. Read the report. It shows the coverage, the cost, and the regions it found.

There is nothing to edit. The script bakes the folder and writes the table that
gives each ship its canopy, so no C++ changes and a drawing cannot go to the wrong
ship.

The script bakes only the drawings that changed. It prints the whole set every time:

    AEGIS     --             no cockpit frame
    LANCE     --             no cockpit frame
    CHARIOT   chariot.png       128 KB of generated header
    BALLISTA  --             no cockpit frame

### A ship with no drawing

A ship with no drawing flies with no cockpit frame. The game supports this. There is
no default texture, and one ship never borrows another ship's canopy.

Everything else still works: the instruments come up, the radio opens, and the ship
flies. What you lose is the frame and the sequence that brings it online, because
both are made from the drawing.

So you can add the four canopies one at a time and fly the game after each one.

### What the report tells you

| line | what to do about it |
|---|---|
| `symmetric` or `asymmetric` | nothing. A symmetric drawing stores half the columns and costs half the flash. |
| `N activation zones ... in order` | make sure the order and the count are the ones you drew |
| `N pixels a frame, N% of the screen` | compare it against the coverage table above |
| `costs the frame about N ms` | an estimate from the device. More than 2.5 ms is too much. |
| `TOO HEAVY` or `HEAVY` | make the shapes narrower |

Ask the board for the real cost after you flash:

    python tools\canopy_cost.py COM6

### Symmetry

The baker looks for left-right symmetry. It does not need it. A cockpit can be
lopsided. A symmetric drawing stores its left half and costs half the flash. A
lopsided drawing stores every column.

### How the frame moves

Each ship moves its cockpit by a different amount. A light hull is looser than a
heavy one. This comes from the airframe, not from the drawing.

Fly a new drawing on more than one ship before you judge it. Some of what you feel
is the ship.

## The regression test

The replay is the only regression test in this project. A session renders frame for
frame. If the same frames come back with the same bytes, the simulation and the
drawing did not change.

Two files make it work, and both are in git:

| file | what it is |
|---|---|
| `captures/regress.phr` | the session. 90 s of play, with the canopy start, a fight, the wall and the rear view in it. |
| `tools/regress-baseline.json` | 9 frame hashes, and the commit they were taken at |

To check a change that must not alter the picture:

    python tools/phantom_regress.py --port COM6 captures/regress.phr --against tools/regress-baseline.json

The render takes about four minutes. The deepest frame in the list sets the time,
because the board must replay every frame before it.

### When to take a new baseline

Take one after any change that is MEANT to alter the picture. Put that change and the
new baseline in the same commit. Do not mix a change that alters the picture with one
that must not.

    python tools/phantom_regress.py --port COM6 captures/regress.phr --frames 300,900,1500,2100,2700,3300,4000,4600,5200 --save tools/regress-baseline.json

### When to record a new session

Record one if `VgInput` changes size. The session stores one input structure for each
frame, so a session recorded against a different size cannot be replayed.

The board refuses it and says so:

    vg_replay: REJECT ver 1 blob 80 (want 1/76)

This is why both files are in git. The baseline used to name a session that had been
deleted, which is the same as having no baseline. Everything else under `captures/` is
still ignored: those are recordings of a moment, not a test.

## Serial commands

Single bytes, typed at the board while nothing else holds the port.

| byte | does |
|---|---|
| `q` | antialiasing on the instruments, on and off |
| `k` | what the canopy costs, rigid and warped |
| `y` | what the backdrop costs, prep and fill, with a checksum |
| `g` | the glyph nest, old against new, over fixed text |
| `l` | the line walk, old against new, over a fan of every slope |
| `x` `X` | fire an explosion, and cycle the preset |
| `b` | link throughput, no rendering |
| `!` | reboot |

`s`, `R`, `E`, `P`, `H` and `A` belong to the capture and replay protocol. Do not
take one for anything else: `s` stops a capture, and a bench that shadowed it
broke recording down to a single frame. Every bench also refuses to run while a
capture or a replay owns the link, because they all draw into the band buffers the
frame being streamed is built in.

Both front ends use `phantom_link`, so there is one copy of the protocol code.
The first version had two bugs of the type that one copy prevents. It looked for
the frame magic inside binary data, and it copied the rotation of the firmware
instead of the inverse.

## Requirements

You need `pyserial`. You also need `numpy` for speed. Without `numpy`, the pixel
conversion runs a Python loop over 230,400 pixels for each frame, and the render
step is very slow.

You need `ffmpeg` on the PATH to write mp4 files. Without `ffmpeg`, the tools
write a sequence of PPM files, and another program can convert them later. The
`.exe` file contains all three.

## Sound

The video contains the sound. The device sends the samples inside each frame,
and the tool writes them into the file. The result is `<name>-av.mp4`. The
`.wav` and the silent `.mp4` stay next to it.

The device makes the sound; the tool does not. There is no second copy of the
synthesiser on this computer, so the recording is what the speaker played.

Two things must agree:

- `AUDIO_RATE` in `phantom_link.py` and `VG_AUDIO_RATE` in `src/vg/vg_port.h`.
  If they differ, the sound plays at the wrong speed. Nothing reports an error.
- The recording must be a REPLAY. During a replay the device makes one frame of
  sound for each frame of picture. It does not use the clock, because the frames
  arrive as fast as the link allows and the clock would stretch the sound
  against its own picture.

The recorded sound is at full level. The volume setting in the game does not
change it, so a recording is not quiet because somebody moved a slider.
