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

There are five presets and each shot steps to the next one:

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
| `canopy_opaque.py` | turns a canopy drawing into a palette and a table the firmware draws |
| `canopy_set.py` | bakes every drawing in `design/canopy/` and writes the table that gives each ship its canopy |
| `canopy_cost.py` | asks the board what the canopy costs to draw |
| `canopy.ps1` | bake, build and flash a new canopy in one command |

## Canopy drawings

The canopy is the cockpit frame. It replaces the crosshair. Each ship can have its
own drawing.

The drawing is opaque: the firmware stores a colour for each pixel of the frame and
paints it over the picture. Most of a cockpit is metal, and metal hides the stars.
The file is `<ship>.png`. Read **An opaque drawing** below for the rules.

An older kind, the light delta, added light to the picture behind it. Its baker and
its renderer were removed on 2026-09-06.

### Bake it

The file name is the wiring. Name the PNG after the ship that flies it.

1. Put the PNG in `design\canopy\`. Use one of these names:
   `aegis.png`, `lance.png`, `chariot.png`, `ballista.png`. Put the tint mask
   beside it.
2. Run `.\tools\canopy.ps1`. It takes no arguments.
3. Read the report. It shows the coverage, the cost, and the regions it found.

There is nothing to edit. The script bakes the folder and writes the table that
gives each ship its canopy, so no C++ changes and a drawing cannot go to the wrong
ship.

The script bakes only the drawings that changed. It prints the whole set every time:

    AEGIS     aegis.png         429 KB of generated header, painted
    LANCE     lance.png         395 KB of generated header, painted
    CHARIOT   chariot.png       388 KB of generated header, painted
    BALLISTA  ballista.png      404 KB of generated header, painted

`painted` means the ship has a tint mask.

### A ship with no drawing

A ship with no drawing flies with no cockpit frame. The game supports this. There is
no default texture, and one ship never borrows another ship's canopy.

Everything else still works: the instruments come up, the radio opens, and the ship
flies. What you lose is the frame and the sequence that brings it online, because
both are made from the drawing.

So you can add the four canopies one at a time and fly the game after each one.

### An opaque drawing

This is what `<ship>.png` is. Most of a cockpit is metal, and metal hides the stars
behind it. The firmware stores one colour for each metal pixel and copies it to the
screen. A copy costs less than a blend, so a drawing of this kind can cover four
times the area of the old light delta for less time.

Name it `<ship>.png`, for example `chariot.png`.

Save the file as RGBA (PNG colour type 6), 480 x 480 pixels or larger. The colour of
a pixel says what it is:

| colour | meaning |
|---|---|
| magenta | a pane. Nothing is stored. The world shows through. |
| cyan | the lit outline. The firmware adds one flat HUD colour to the picture behind it. |
| any other colour | metal. The firmware stores the colour and paints it over the picture. |

The baker reads magenta and cyan by hue, not by level, so any bright magenta or cyan
works. Draw the outline one or two pixels wide. The baker does not thicken it.

The alpha channel holds the activation regions, one flat value per region, over the
whole image. The lowest value comes on first. A region must cover at least 0.2% of the
image, or the baker treats it as an edge and merges it into its neighbour.

The baker writes a palette of 256 colours. Colours outside the palette snap to the
nearest entry, and the report shows the error. The CHARIOT's opaque drawing is
116 KB of flash. Four opaque drawings are about 460 KB.

### The player's colour on the cockpit

A ship with an opaque drawing can also have a tint mask, `<ship>_tint.png`. It says
which metal takes the colour the player chose for their trail. The rest stays as
drawn.

Draw the mask as a greyscale PNG, the same size as the drawing. White is painted.
Black is bare. The baker uses a hard edge at the halfway point, so a soft boundary in
the mask becomes a crisp line in the paint.

The mask is its own file because the drawing has no channel left. Three channels hold
the paint and the alpha channel holds the activation regions.

**This costs no frame time.** The firmware reads each metal pixel through the palette.
The baker gives the painted metal its own range of palette entries, which bare metal
never uses. To paint the cockpit the firmware writes those entries once, when the
player picks a colour. It does not touch a pixel.

Two rules follow from that:

1. The mask must cover some metal, but not all of it. The baker stops with an error
   if it covers none or all.
2. The split gives each range fewer colours. The baker reports the error, and the
   split is in proportion to the pixels.

Keep the mask off the lit outline. The outline is the wall warning, and it must mean
the same for every player.

The look is set by three values in `src/vg/cfg_hud.h`:

| value | does |
|---|---|
| `CANOPY_TINT_KEEP` | how much bare metal is mixed back. 0 paints fully. 1 does not paint. |
| `CANOPY_TINT_EVEN` | how far every hue is moved to one brightness. 0 keeps each hue's own. |
| `CANOPY_TINT_LUMA` | the brightness they are all moved to |
| `CANOPY_TINT_GLOSS` | how much the highlights go white. 0 is matte. 1 is a wet gloss. |
| `CANOPY_TINT_GLOSS_AT` | how bright the metal must be before it starts to go white |
| `CANOPY_TINT_EDGE` | how far the lit outline takes the colour too. 0 keeps it amber. |
| `CANOPY_TINT_EDGE_LIFT` | how far that outline colour is lifted toward white to keep its brightness |

A fully saturated hue does not carry a constant brightness. Yellow is nearly eight
times as bright as blue. Without `CANOPY_TINT_EVEN` a red ship looks lit and a blue
ship looks unlit, on the same drawing. Evening the brightness costs saturation in the
dark hues, because a dark hue can only get brighter by taking white.

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

## What a drawing costs

`replay_cost.py` measures the CPU the game spends, over a recorded session. Use it to
compare two canopy drawings, or to check that a change did not make the frame slower.

    python tools/replay_cost.py captures/regress.phr --port COM6

A run takes about 20 seconds. The device replays the session and sends no pixels.

### Why not simply fly and read the telemetry

Because the numbers move. The `can` counter was measured at 2805 microseconds and again
at 7100 in the same fight, in the same build. The fight is different every second, so two
readings from two fights compare the two moments and not the two drawings.

A replay is the same frames every time. The only difference between two runs is the
build. Measured: the same build twice gives the same `can` to the microsecond.

### To compare two drawings

1. Put the first drawing in `design/canopy/`. Run `tools/canopy.ps1`.
2. Run `replay_cost.py` with `--save first.json`.
3. Put the second drawing in. Run `tools/canopy.ps1` again.
4. Run `replay_cost.py` with `--against first.json`.

The table shows both runs and the change between them.

### What the numbers mean

| name | what it measures |
|---|---|
| `can` | the cockpit |
| `rast` | the whole raster: sky, primitives and scanlines |
| `prim` | the primitives alone |
| `sub` | building the primitive list |
| `upd` | the simulation |

Compare `rast` with **11520 microseconds**. That is the time the wire needs to send one
frame, and the frame cannot be quicker than it. Below that number, more drawing is mostly
free, because the processor waits for the wire anyway. Above it, the processor sets the
frame time, and every microsecond of drawing costs a microsecond.

The report also prints two diagnostic lines. The `CACHE` line counts the cache
misses of each frame: `im` is instruction misses, `dfm` is data misses to flash,
and `dpm` is data misses to PSRAM. The hash line (`PRIMH`, `RNGH`, and the
others) is a checksum of the simulation. Two runs of the same build must show
the same hashes. If the hashes differ, the change altered the simulation and
the cost numbers compare two different scenes.

**This tool does not measure the frame rate.** It sends no pixels to the computer, and
the numbers are processor time. The panel still runs, and the `push` figure is the
time the processor waited for it.

### Options

| option | does |
|---|---|
| `--frames N` | stops after N frames. Use the same N for every run you compare. |
| `--save FILE` | writes the result to a JSON file |
| `--against FILE` | compares with a saved result |
| `--warp flat`, `full`, `rigid` | holds the bend of the frame at none, at full, or off with the lag as well. Without it, the recorded throttle drives the bend. |
| `--resident` | reads the opaque drawing from a copy in PSRAM. This is the default in flight. |
| `--hash` | folds the pixels of every 256th frame into one number on the board. Two builds that draw the same picture give the same number. The times of such a run are not valid. |

The tool sends each choice to the board and waits for the board to name it back. If
the board does not answer, the tool stops. A run that cannot say what it measured has
nothing to compare.

The tool measures a fight from a session that starts with the menus and the course.
To measure the fight alone, run once to a frame before the fight and once to a frame
after it, then subtract the first from the second.

## The regression test

The replay is the only regression test in this project. A session renders frame for
frame. If the same frames come back with the same bytes, the simulation and the
drawing did not change.

Two files make it work, and both are in git:

| file | what it is |
|---|---|
| `captures/regress.phr` | the session. 90 s of play, with the canopy start, a fight, the wall and the rear view in it. |
| `tools/regress-baseline.json` | 9 frame hashes, and the commit they were taken at |

### Every baseline in `tools/`, and which ones are live

A baseline names the commit it was taken at. Compare only against a live one. The
historical ones describe a build that no longer exists, and are kept as evidence, not
as tests.

| file | session | tool | status |
|---|---|---|---|
| `regress-baseline.json` | `captures/regress.phr` | `phantom_regress.py`, 9 frames | stale, see below |
| `surge-baseline.json` | `captures/phantom-20260809-221925.phr` | `phantom_regress.py`, 9 frames | stale, see below |
| `course-baseline.json` | `captures/course.phr` | `replay_cost.py` | stale, see below |
| `ordnance-baseline.json` | `captures/ordnance.phr` | `replay_cost.py` | stale, see below |
| `anomaly-baseline.json` | `captures/phantom-20260809-221925.phr` | `phantom_regress.py` | historical: the build before the surge event, commit 950ebf0 |
| `regress-baseline-exact.json` | `captures/regress.phr` | `phantom_regress.py`, 4 frames | historical: commit 9f971da |
| `course-iram-baseline.json` | `captures/course.phr` | `replay_cost.py` | historical: the IRAM milestone, commit 5f99ebd |

**Every recording in `captures/` is older than the menus.** A replay reaches the game
by the taps the recording holds, and a tap is a screen coordinate. The console
chassis and its keys moved on 2026-09-03 and 2026-09-04, and every recording is from
August. On a build from 4 September on, each of these recordings stops at the
callsign screen and stays there: all nine frames of `regress.phr` that the test
samples, 300 to 5,200, are that screen. The replay tools now count the frames that
drew a cockpit and refuse to save a baseline when there are none.

So the four stale baselines describe the game as it was in August, and they cannot be
retaken until the four sessions are recorded again on the current menus. Record them
with `PhantomRecorder.exe` and give each file the name in the table.

To check a change that must not alter the picture:

    python tools/phantom_regress.py --port COM6 captures/regress.phr --against tools/regress-baseline.json

Use a Python that has numpy. The PlatformIO Python does not. Without numpy the tool
decodes the picture slowly, its receive thread falls behind, and the port loses
bytes with no error. The tool refuses to run without numpy and names the Python it
ran under. Do not build or run other programs while a render runs.

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
| `f` `F` `i` `n` | hold the bend of the frame flat, full, or rigid with the lag off. `n` gives the throttle back. |
| `r` `h` | `r` reads the opaque drawing from PSRAM (the default), `h` from flash |
| `w` | arms the picture hash for the next timed replay, and turns it off again. The setting survives a reset. |
| `d` | prints the full breakdown once, on the next telemetry line |
| `c` | prints the cost of the last timed replay again |
| `k` | what each ship's canopy costs, rigid and at full bend, one core |
| `y` | what the backdrop costs, prep and fill, with a checksum |
| `g` | the glyph nest, old against new, over fixed text |
| `l` | the line walk, old against new, over a fan of every slope |
| `x` `X` | `x` fires one explosion and steps to the next preset. `X` turns the repeat on and off. |
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

## Windows blocks the PowerShell scripts at first

`canopy.ps1` fails the first time you run it on a new machine:

    .\canopy.ps1 : File ...\canopy.ps1 cannot be loaded because running scripts
    is disabled on this system.

Windows does not run PowerShell scripts by default. The script is correct. Run this
command one time to allow your own scripts:

    Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned

You do not need administrator rights. `RemoteSigned` runs the scripts you wrote and
still blocks a downloaded script that has no signature. The setting stays after you
close the window.

To run the script one time and change no setting:

    powershell -ExecutionPolicy Bypass -File .\canopy.ps1

If your company sets this policy for you, `Get-ExecutionPolicy -List` shows
`MachinePolicy` or `UserPolicy`. The command above cannot change those two. Use the
`-ExecutionPolicy Bypass` form instead.

Also give the script a path. PowerShell does not run a script from the current
directory on the name alone:

    .\tools\canopy.ps1        from the top of the repository
    .\canopy.ps1              from the tools folder

`canopy.ps1` with no `.\` fails with `CommandNotFoundException`. The script itself
runs from any directory: it finds the repository from its own location.

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

## Music

`tools/score.py` builds the game's music from a score file. The score files are in
`design/score/`.

    python tools/score.py design/score/motif.score
    python tools/score.py design/score/motif.score --mid motif.mid
    python tools/score.py design/score/motif.score --sheet motif.txt
    python tools/score.py design/score/motif.score --abc motif.abc
    python tools/score.py design/score/motif.score --bake
    python tools/score.py --from-mid some.mid

The tool prints a report. It can also write a MIDI file to listen to, and a table
for the device to play. `--from-mid` reads a MIDI file and prints a score file, so
you can bring in music that you wrote somewhere else.

`--abc` writes the whole arrangement as one ABC tune. It follows the order, so a
section that plays twice is written twice, and it holds every part: one voice for
each pitched part, and one percussion staff for the drums. MuseScore reads ABC, and
`abc2xml` turns it into MusicXML. Use `--key` to set the key. The default is the
minor key of the first root.

`--sheet` writes a text score. Give the file to a person, or to a tool, that makes
sheet music. It writes each section one time, and it gives the order at the end.
It gives the WRITTEN length of each note. The MIDI file gives the SOUNDING length,
which is shorter, because each part has a gate.

### The score file

A score file holds settings and parts.

| Setting | Meaning |
|---|---|
| `name` | the name of the score. `--bake` uses it for the file name. |
| `tempo` | beats for each minute |
| `cell` | beats in one group |
| `reps` | the number of times each group repeats |
| `roots` | one root note for each group, in order |
| `transpose` | move every note by this number of semitones |

A part line holds the settings of one voice, then a colon, then the pattern.

    part lead  square gain 0.26 lp 2800 gate 0.55 sus 0.20 : R R D4 D#4

The wave is `square`, `sine` or `noise`. The other settings are columns of
`SynthLayer` in `src/vg/vg_synth.h`. `gate` is the part of a slot that sounds.

The pattern is a list of slots. The slots divide one group into equal steps. A
pattern of 8 slots is twice as fast as a pattern of 4 slots.

| Slot | Meaning |
|---|---|
| `R` | the root of this group |
| `R-12` | the root, one octave lower |
| `D#4` | this exact note, in every group |
| `.` | a rest |
| `-` | hold the note before it for one more slot |

`--from-mid` does not find a tie. It reads a held note as one long note, and it
gives the part a large `gate`. Put the ties back by hand.

### Sections, to build a song

A score with no `section` line is one section, and it repeats. To build a song,
give it sections and an order.

    part lead  square ... : R R D4 D#4      <- the base. Every section plays it.
    part kick  square ... : G2 . . .

    section verse
        free melody square ...
            at 0  C5 6.25

    section high
        transpose 12
        free melody square ...
            at 0  G4 4

    order       verse verse high verse

Three rules govern it.

- A section plays the BASE parts, which are the parts written before the first
  `section` line. Put the beats and the lead there and they run through the whole
  song.
- A part inside a section REPLACES a base part with the same name. That is how
  one section gets its own melody over the same beats.
- `order` is the arrangement. A name may appear as often as you want. Two turns
  of one section are the same section, so an edit to it changes both.

A section can also set `transpose`, `reps` and `roots` of its own. A section with
its own roots plays in another key. A field it does not set comes from the score.

### Build a song in the window

The strip under the buttons is the order, one box for each turn. Click a box to
edit that turn. Clicking the grid picks the turn it lands in.

### Which turns are linked

Two turns of ONE section are the same music. An edit to one changes both, and
that is the point of the order: write a chorus once and play it four times.

The strip shows this.

- Every turn carries the colour of its section, so two turns of one section look
  the same.
- The turns of the section you are editing are bright. The rest are dim.
- The selected turn has a white border.
- The grid puts a faint wash over every turn you are about to change, and marks
  a repeated section `linked`.
- The line beside the strip says it in words, such as
  `main2 plays 2 turns. One edit changes them all.`
- The report names them, such as `LINKED: turns 2, 3`.

A section that plays once says `plays once. It is on its own.`

| Button | What it does |
|---|---|
| `New turn` | adds a turn playing a NEW, empty section. The base parts play in it and its free parts are blank, so there is somewhere to write something that is not a version of anything. |
| `Duplicate` | copies this section under a new name and plays it next. THE COPY GETS ITS OWN FREE PARTS, so a melody can be rewritten at once. Pattern parts stay shared, so the beats and the lead run on. |
| `Repeat` | plays the SAME section again. An edit changes both turns. |
| `Skip` | keeps the turn in its place but does not play it. Use it to hold a section back for later. |
| `Remove turn` | takes one turn out of the order. The section itself is kept. |

A skipped turn makes no sound and takes no time. Its box shows the name in
brackets and the grid leaves it out. In the file its name carries a minus, such
as `order main -main22 drift`, so it survives a save.

The three that add a turn are three different things, and it is worth keeping
them apart. `Repeat` plays the SAME section again, so an edit reaches both turns.
`Duplicate` copies it with its notes, to vary something that already works.
`New turn` gives a blank one.

DRAG A BOX ALONG THE STRIP to move that turn to another place in the order. The
boxes move as you drag, so you see the new order before you let go. One drag is
one undo step.

Only the order changes. The sections and their notes are untouched, so a turn
carries its music with it.

The part list shows `base` or `local` for each part. A base part is one object
that every section plays, so an edit to it reaches the whole song. `Make this
part local to the section` gives the section its own copy, and after that an edit
to it changes nothing else.

`Add` follows the same idea. A free part goes in the section you are editing,
because a melody is what changes. A pattern part goes in the base, because the
beats run on.

### A free part

A pattern part repeats with the groups. A free part does not repeat. Use a free
part for a melody.

    free melody square gain 0.20 lp 4200 gate 0.95 sus 0.35
        at 0        G4    4
        at 4        F4    4

An `at` line gives a beat from the start of the score, a note, and a length in
beats. Every `at` line belongs to the `free` line above it.

### The window

`tools/score_studio.py` shows the notes on a grid. `tools\studio.cmd` starts it.

    tools\studio.cmd
    tools\studio.cmd design\score\motif.score

Double click `tools\studio.cmd`, or run it from PowerShell, from cmd or from
bash. Run it from any directory. It opens `design/score/motif.score` when you
give it no file. It uses `pythonw`, so it opens no console window.

To start the studio without the launcher, use Python:

    python tools/score_studio.py
    python tools/score_studio.py design/score/motif.score

Pick a part in the list, then edit on the grid.

| Mouse | Result |
|---|---|
| left click on an empty cell | add a note |
| left drag on a note | move the note |
| left drag on the right edge of a note | hold the note longer |
| RIGHT DRAG across the grid | pick every note in the box |
| left drag on a picked note | move every picked note together |
| shift and that drag | move them in time only, and keep their pitches |
| right click on a note | remove the note |
| the mouse wheel | zoom in and out |
| control and the wheel | scroll up and down |
| shift and the wheel | scroll left and right |

The wheel zooms around the beat under the pointer, so the bar you are looking at
stays where it is. The wheel does what the axis under it does: over the note
names on the left it scrolls the pitches, and over the band of times at the top
it zooms.

The pointer changes shape over the right edge of a note. Drag that edge to the
right to hold the note longer. Drag it to the left to make the note shorter.

`Undo` and `Redo` go back and forward through the edits. The keys are control
and z for undo, and control and y for redo. Control and s saves.

One drag is one step. A drag sends many events, and each one changes the score,
but undo goes back to the state before the drag started. A drag that changes
nothing adds no step. Undo holds the last 100 steps.

Undo covers the notes and the parts. It does not cover the file. To go back to
the file on disk, open it again.

### Tempo and volume

The tempo slider is in the row of buttons. It changes the speed of the whole
score, from 40 to 200 beats for each minute. The notes keep their beats, so
nothing moves against anything else. Only the length in seconds changes.

The volume slider is under the part list. It changes the volume of the part you
picked, and no other part. The list shows the volume of each part on its line.
The volume is the `gain` column of `SynthLayer`, so a change goes to the file
and to the device.

One drag of a slider is one undo step, in the same way a drag of a note is.

### Add a part from a preset

Pick a kind next to `Add part`, then press it. The preset gives the part settings
that work, so you do not have to find them again.

A PRESET GIVES THE SOUND AND NOTHING ELSE. Every part arrives empty. The timing
is yours, and so is the SHAPE: the box beside the preset chooses it.

| Shape | What it does |
|---|---|
| `free` | every note stands on its own. Nothing repeats. Use this unless you want a repeat. |
| `pattern` | a cell of slots that repeats in every group and every rep of the section |

A pattern part is a small machine: one cell, played 16 times in a section of 4
groups and 4 reps. That is right for a hat and wrong for a bass line that has to
change. When in doubt, take `free`.

`Stop this part repeating` writes a pattern part out as separate notes. The notes
are the same and it sounds the same, but nothing repeats afterwards, so each note
can be moved on its own. There is no way back, other than undo.

A new part goes in the BASE, whatever its shape, so every section plays it. Use
`Make this part local to the section` to narrow it to one.

| Preset | The sound it gives |
|---|---|
| `melody` | a free part, for a tune that does not repeat |
| `bass` | a square with a low corner, for `R-12` slots |
| `kick` | a square that falls 18 semitones. NOT a low note |
| `snare` | noise with a middle corner |
| `hat` | quiet noise, 8 slots, so one slot is a sixteenth |
| `chord` | the sound of the motif part |
| `empty` | a plain square |

`score.PRESETS` in `tools/score.py` holds the settings. The drum numbers come
from `vg_sfx.cpp`, which was tuned by ear on the device. `score.HINTS` holds the
line the window prints when you add one.

A second part of the same kind gets a number, such as `kick2`.

### Start a new song

`New` clears the window and starts a blank score: four roots, one free part, and
nothing written. It asks first, because anything not saved is lost. Use `Save as`
to put it in `design/score/`.

### Make a variant of a part

`Copy this part, to make a variant` copies the part you picked, notes and all,
under a new name. The copy lands where the original lives: a base part stays base
and a part of a section stays in that section.

Use it for a second lead or a second set of drums. Copy, make the copy local to
one section, then change it there. The original keeps playing everywhere else.

### How long a turn is

`groups` sets how many roots a turn plays, and `reps` how many times each group
repeats. A turn is `groups` times `reps` times `cell` beats long.

A section that sets its own `roots` or `reps` keeps the change to itself. One
that does not is reading the score's, so the score's are what change, and every
other section that has none of its own changes with it. The status line says
which happened.

A new root starts one semitone under the last. Drag a green note to move it.

### Change the timing grid

`slots in a group` sets how many slots a pattern part has. The slots divide one
group into equal steps, so 4 slots give eighth notes when the cell is 2 beats,
and 8 slots give sixteenths.

Each part has its own count. A drum can run in sixteenths while the bass runs in
quarters. A free part has no slots, so the box is off for one.

To change the count, pick the part and pick a number. The notes already written
move to the nearest new slot.

### Remove a part

Pick the part and press `Remove`, or right click its row. Undo puts it back.

A score needs one part, so the last one cannot be removed.

### Turn a part off to hear the rest

Every part has TWO boxes beside its name. The first box is the sound. The second
box is the drawing. They work on their own, so a part that you cannot hear is
still on the grid, and a part that you cannot see still plays.

Clear the first box and the part stops sounding. Use it to hear one part on its
own, or to hear what the others do without it.

This is a listening control.

- It changes no note.
- It makes no undo step, and an undo does not switch parts back on.
- It is NOT written to the score file. A file you open always sounds in full.
- The part stays on the grid, in grey, so you can see it and switch it back on.

If the score plays when you press the box, it plays again at once with the
change.

### Hide a part to read the one under it

Clear the SECOND box and the part is left off the grid. Two parts often sit on
the same rows, and the notes of one hide the notes of the other. Hide the part
you are not working on.

This is a drawing control.

- It changes no note, and it makes no sound.
- It makes no undo step, and it is NOT written to the score file.
- You cannot click a note you cannot see, and a right drag does not pick one.
- If you hide the part you are editing, the studio moves you to the next part
  that is in view. A part you cannot see is a part you cannot edit.

The name of a part goes grey when the part is off OR out of view.

WARNING: `--bake` leaves out a part that is off. The header says which parts it
left out, and `tools/score.py` prints a warning. Switch every part back on before
you bake a score for a build.

### There is no scale

The tool holds no key and no scale. Any of the twelve semitones goes anywhere,
in any part. Nothing is quantised and nothing is corrected.

A NOISE PART IS DIFFERENT, and it is not a restriction. Noise has no pitch, so
`SynthLayer` ignores the frequency of a noise layer and every hit sounds the
same. A noise part gets a strip of its own under the pitched rows, named after
the part. Click anywhere along that part's strip and the height is ignored.

A part that SWEEPS never follows the root. A kick clicked on the root of a group
is written as a note name, not as `R`, because a drum that changed pitch with
each group would be four different drums.

### How high the grid goes

The grid always covers C1 to C7. It is not built from the notes that are already
there, so you can always write a note higher or lower than anything in the score.

The window scrolls to your notes when it opens a file. Most music uses a small
part of the range, so the rest is below and above the view.

### Pick a group of notes

Drag with the RIGHT button to draw a box. Every note in the box is picked, and
picked notes are drawn with a white edge. Drag any one of them with the left
button and they all move together. Hold SHIFT during that drag and they move in
time only, keeping the pitch each one has.

A left click on an empty cell clears the pick. A right CLICK, with no drag, still
removes one note, as before.

| Key | What it does to the picked notes |
|---|---|
| backspace, or delete | removes them |
| control and c | copies them |
| control and v | pastes them at the playhead |

A paste goes into the section the PLAYHEAD is in, into the free part with the
same name as the part it came from. The notes keep their spacing and their
pitches, and the first one lands on the playhead. They are picked after a paste,
so one drag moves them again.

If that section has no free part of that name, the paste says so and writes
nothing.

Undo clears the pick. An undo builds the score again, so the notes that were
picked are new objects and the old pick would point at nothing.

Only the notes of a FREE part can be picked. A slot of a pattern part is shared
by every group of every turn, so there is no one note to move. The report line
says how many were left out.

### What a colour on the grid means

A slot of a pattern part is one of two things. Dragging them does two different
things, so the colour says which one you have.

| Colour | What it is | What a drag does |
|---|---|---|
| green | a slot written as `R`, which follows the root of its group | moves the root of THAT group. The other groups stay. |
| blue | a slot written as a note name, such as `D4` | changes that slot in EVERY group, because the groups share one pattern |
| orange | a note of a free part | moves that one note |
| purple | a hit of a noise part, in its own strip | nothing. A hit has no pitch to move |

To move a root, drag a green note. That is the only way to change a root on the
grid, and it is what you want when a group should start on a different note.

Every part that follows the root moves with it. That is the point of a root.

An edit to a free part changes one note.

The two kinds of part hold a note in different ways.

- A free part keeps a length in beats for each note, so a note can be any length.
- A pattern part has slots. To hold a note, the tool puts a hyphen in the slots
  that follow it. A note cannot hold past the end of its group, because the
  next group starts the pattern again. The status line says so if you try.

The report at the left gives the length, the voice count, the speaker check and
the harmony. It changes as you edit.

### The engine starts with the window

The window opens the sound engine as soon as it appears, not on the first `Play`.
Opening the sound card takes a moment, and doing it at the start means the first
`Play` and the first click on a note are immediate.

The engine opens after the window is drawn, so a build never holds the window
back. If it cannot start, the status line says so and the window still works.
`Play` tries again.

### It plays live

The window does not render a file and play it. `tools/host/score_live.exe` runs
`src/vg/vg_synth.cpp` in real time and takes commands while it plays, so nothing
below stops the sound.

- Clear the box of a part and the part stops on the next buffer, about a
  hundredth of a second. The engine holds every note of every part and decides
  when each note starts, so nothing is worked out again.
- Edit a note and the change reaches the next turn of the loop. A drag sends
  nothing until you let go, because the note you drag is sounded on its own.
- Click a note and you hear it at once. `Hear clicks` turns that off.
- Move the loop switch or the volume and the sound keeps running.

The playhead follows the engine, which reports its position ten times a second.

### The transport

| Button | What it does |
|---|---|
| `Play` | plays from the playhead. After a pause it goes on from there. |
| `Pause` | silences the sound and leaves the playhead where it is |
| `Stop` | silences the sound and puts the playhead back at the start |

The SPACE bar plays, and plays again to pause. Control and s saves, and it saves
from anywhere, a box included.

The grid holds the keyboard. A box or a button takes it while you use one, and
gets it back to the grid as soon as you are done, so space keeps working. If a
box does still hold it, the status line says so and a click on the grid fixes it.

Copy, paste, backspace and delete all say so too. Before, they did nothing and
said nothing, and the status line still showed the last message. A copy that
looked like it worked left the EARLIER notes in the clipboard, and the next
paste put those back.

The REPORT does not hold the keyboard. You cannot type into it, so a click on it
to read a line no longer stops the keys the grid owns.

The MIDDLE mouse button on the grid puts the playhead where you click. Use it to
play one part of a long song without waiting for the rest. It works while the
song plays and while it is stopped.

The playhead is drawn when the sound is stopped as well, so you can always see
where `Play` will start. The status line names the turn and the bar.

Only two things start the sound again: `Play`, and a change of tempo.

WARNING: the synthesiser has 10 voices. A note you click takes one, the same as
a note of the score. The device does the same, so what you hear is honest.

### Hear it as a file

`tools/score_audio.py` renders a WAV file with the same synthesiser. Use it for
an export. `Play` uses the live engine instead. `tools/score_audio.py` builds
`tools/host/`, which compiles `src/vg/vg_synth.cpp`, the file the firmware runs.

    python tools/score_audio.py design/score/motif.score
    python tools/score_audio.py design/score/motif.score --speaker

The build needs the C++ tools of Visual Studio 2022. The build runs once. After
that it runs again only when a source file changes.

`tools/host/` is not `host/`. `host/build.ps1` builds the whole game for the
desktop, with a compat layer for the board. `tools/host/` takes one game source
file, `src/vg/vg_synth.cpp`, and adds a main program that writes a WAV file. The
music must not touch the simulation, so the renderer does not compile it.

`Loop` plays the score again and again. Windows does the loop itself, so there is
no gap. The score is music that repeats, so use the loop to judge it.

A loop stops at the end of the last group. Three things follow from that.

- The file carries no tail, because a tail would be a gap in the loop.
- A note that runs past the end of the last group is cut, because it would
  sound over the start of the next turn of the loop.
- The last three milliseconds fade to zero. A note that is cut still sounds at
  full level, and the step from that level back to silence is a click.

`Stop` ends the loop. On the command line, use `--loop`.

WARNING: `Play as device` and `--speaker` add a filter that APPROXIMATES the
device driver. Nobody measured the driver to make that filter. Use it to find
notes that the device loses. Do not judge level or tone from it.

### A bass part and a drum part

A part is a track. One part makes one sound, so a drum kit is three parts. Give
the kick, the snare and the hat a line each.

For a bass, use a pattern part and put the root an octave down.

    part bass  square gain 0.30 lp 900 gate 0.70 sus 0.30 : R-12 . R-12 .

For a drum that has no pitch, use the noise wave and the slot `X`. `X` is a hit.

    part hat   noise gain 0.12 lp 7000 gate 0.10 sus 0 atk 0.001 : . X . X

A kick drum is different, and the reason matters. Do NOT make a kick from a low
note. The speaker is one centimetre across and a low note alone gives nothing.
Weight comes from a tone that FALLS fast. `sweep` gives the number of semitones
the pitch falls across the note.

    part kick  square gain 0.55 lp 700 gate 0.50 sus 0 atk 0.002 sweep -18 : G2 . . .

That is 98 Hz down to 35 Hz. `vg_sfx.cpp` builds its explosion the same way, at
90 Hz down to 28 Hz, and says why: "the falling tone is what a small speaker
turns into weight".

`design/score/example-beats.score` is a working example of all three, with a
bass and a melody. Open it, play it, and take what you want from it.

WARNING: every part you add costs voices. `vg_synth` has 10 and no priority.
Read the voice count in the report after you add a drum. In the example, hats on
every slot made 5 voices, and hats off the beat made 4.

### What a bass can be on this device

The driver is one centimetre across and gives very little under 300 Hz, so a low
note is never heard as itself. It is heard through its HARMONICS. A square wave
at f also holds energy at 3f, 5f and so on, and the ear puts the missing
fundamental back.

These numbers were measured through the game's own synthesiser.

| Sound | Energy above 300 Hz | What survives the driver |
|---|---|---|
| square 104 Hz, lp 900 | 12% | 23% |
| square 104 Hz, lp 250 | 3% | 14% |
| square 52 Hz, lp 900 | 3% | ... |
| sine 104 Hz, lp 900 | 0% | 11% |

Three rules follow.

- USE A SQUARE, NEVER A SINE. A sine has no harmonics, so nothing carries it. It
  measured 0% of its energy in the band the driver can move.
- KEEP `lp` ABOVE THREE TIMES THE NOTE. At 104 Hz the third harmonic is 312 Hz,
  and that one partial is the whole sound. `lp 250` cuts it and the note goes.
- ABOUT 100 Hz IS THE FLOOR. Under it the third harmonic falls under 300 Hz too
  and there is nothing left to carry the note. G#2 at 103.8 Hz is the lowest
  note that works, because 3 times 103.8 is 311.5.

| Note | Hz | 3rd harmonic | Usable |
|---|---|---|---|
| C2 | 65.4 | 196.2 | no |
| F2 | 87.3 | 261.9 | no |
| G#2 | 103.8 | 311.5 | yes, the floor |
| C3 | 130.8 | 392.4 | yes |
| G3 | 196.0 | 588.0 | yes |

A DRUM IS DIFFERENT. It gets its weight from a pitch that FALLS fast, not from a
low pitch held. See the `kick` preset and `sweep`.

### Read the speaker check first

WARNING: the device speaker is one centimetre across. It gives very little under
300 Hz. The report marks a note that is too low as `thin` or `SILENT`.

A square wave under 300 Hz is still audible, because its harmonics carry it. A sine
wave under 300 Hz is not audible. For a low square, keep `lp` above three times the
frequency of the note. Below that the low pass removes the harmonics, and the note
goes silent.

To lift the whole score, add a `transpose` line.

### Read the voice count

WARNING: `vg_synth` has 10 voices and no priority. It takes the voice with the least
time left. The music and the missile alert compete for the same 10 voices.

Keep the peak count at 4 or less. Above that the music takes a voice that an alert
needs.

### The device has no player yet

`--bake` writes `src/vg/generated/score_<name>.h`. The firmware cannot play that
table. `VgVolume.music` in `src/vg/vg_sfx.h` has no consumer either. Write the
player before you put a score in a build.

### There is one synthesiser, and it is built twice

There is still no second copy of the synthesiser on this computer. `tools/host/`
compiles `src/vg/vg_synth.cpp` itself. The preview cannot drift from the firmware,
because it is the firmware code.

WARNING: do not write a synthesiser in Python here. A second copy drifts from the
first, and then the preview lies.

The MIDI file is a different thing. It uses a piano, so it gives you the notes and
not the timbre. Use it to judge the notes on a computer that has no compiler.

The preview cannot give you the speaker. `--speaker` only approximates it. The
device is the one true test of tone.
