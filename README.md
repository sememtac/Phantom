# PHANTOM

A wireframe dogfight game for the **Waveshare ESP32-S3-Touch-AMOLED-2.16**. The board
has a round 480x480 AMOLED screen, a touch layer, an accelerometer and a speaker. The
game writes to the screen through the QSPI bus. It does not use a widget toolkit.

You fly a tournament of missile duels, one pilot against one pilot. Sixteen pilots
start and one wins. The last opponent is the pilot the opening story is about.

The game makes everything you see and hear while it runs. The ships, the arena, the
sky, the canopy and every sound are generated. There are no bitmaps and no samples.

- About 22,000 lines. Only one library is not written here.
- 60 to 61 frames a second in a fight, 60 to 64 on the ring course, and 71 when idle.
- The game can record itself and play a session back, frame for frame.

---

## How to play the latest

One script builds and runs the game, on the PC or on the board:

| command | what it does |
|---|---|
| `.\play.ps1` | build the PC version and run it |
| `.\play.ps1 device` | build the firmware and write it to the board |
| `.\play.ps1 both` | build both, write to the board, then run the PC version |
| `.\play.ps1 check` | build both and stop. Nothing runs and nothing is written. |

Use `check` after you change anything in `src/`. Both builds share every line of
`src/`, so a change can build for one and fail for the other.

The script also does three things you must otherwise remember. It sets
`PYTHONIOENCODING`, without which an upload can stop and never continue. It finds
the board instead of assuming a port. It warns you if the tray daemon is running,
because that daemon restarts the board while you play.

If two boards are attached, name one once with `-Port COM6`. The script remembers
it.

See `host/README.md` for the PC version on its own.

## How to put the game on the board

You need [PlatformIO](https://platformio.org/). The first build downloads the board
profile, the toolchain and the one library.

You can use PlatformIO directly. `play.ps1` runs these commands for you.

| command | what it does |
|---|---|
| `pio run` | build |
| `pio run -t upload` | build, then write to the board |
| `pio device monitor` | show the telemetry |

PlatformIO finds the port. If more than one board is connected, give the port:
`pio run -t upload --upload-port COM6`.

### Before you upload

Set `PYTHONIOENCODING` to `utf-8` first. If you do not, the upload can stop and never
continue. A flash that stops in the middle leaves a board that does not start.

    set PYTHONIOENCODING=utf-8            in cmd
    $env:PYTHONIOENCODING = "utf-8"       in PowerShell

Do not stop an upload. `esptool` checks the hash at the end. To repair a board that
was interrupted, erase all of the flash and write it again.

### If the board runs other firmware

An upload replaces everything on the board. There is no dual boot, and no partition
keeps the other firmware safe.

Close every program that uses the serial port before you upload. A monitor, a
background service or an IDE all hold the port. Only one program can hold it. The
telemetry monitor holds it too.

If the port is held, the upload stops and shows `Access is denied` or
`could not open port`.

### What is on the board

| part | function | driver |
|---|---|---|
| CO5300 | 480x480 AMOLED screen, QSPI at 80 MHz | `vg_port_co5300.cpp`, written here |
| CST9220 | touch, up to 5 contacts | SensorLib |
| QMI8658 | accelerometer at 250 Hz | SensorLib |
| ES8311 | mono codec and speaker, I2S | `vg_audio` in the port |
| AXP2101 | power management and the power key | `vg_pmu_*` in the port |

The screen driver is written here because the transfer is nearly all of the time a
frame has. A general display library cannot give one band to DMA and return at once.
See **Speed** below.

---

## The parts of the game

### Controls

| input | action |
|---|---|
| left edge strip | throttle. It moves to your thumb. |
| anywhere to the right | steering. Touch to set an origin. Hold away from the origin to turn. Lift to centre. |
| **BOOT** button | fire one missile at the locked target |
| hold the **+/-** key | the steering swipe rolls the ship instead of turning it |
| hold the rear-view window | look behind. This changes the view only, not the ship. |
| **PWR** | pause, and the menus |

Steering points the nose where your finger goes. Move the finger up and right and the
ship aims up and right. To reverse this, change `STEER_PITCH_SIGN` in `cfg_flight.h`.

**The screen does not fire.** A missile goes only when you press the BOOT button. The
game fires on the press, not while you hold the button, so one press sends one missile.
If it fired while held, the rack would be empty in three seconds. The lock takes time to
get, so a shot has to be a decision.

The **+/-** key does two things. In flight, hold it and the steering swipe rolls the ship.
In a menu it is a menu key. The game decides which, not the input code.

The steering is a screen joystick, not tilt. Tilt was built first and then removed:
you hold the board at whatever angle is comfortable, and the code cannot know that
angle. It measures a neutral pose once for each run, but any change of grip makes the
measurement wrong. The accelerometer code is still there. Set `STEER_MODE` to 2 for
tilt, or to 1 for relative steering.

Roll has its own key because there is no other way to roll. Roll is what makes a turn
hard to predict. The same key also opens menus. It means one thing in flight and
another thing in a menu, and the game decides which.

### Flight and missiles

One rule causes most of what happens: a missile can turn only so many degrees each
second.

- A missile aims at where the target will be, not at where it is. This is what curves
  the path.
- The seeker looks through a cone. If the target leaves the cone, the lock breaks and
  does not come back. The missile then flies straight. Nothing in the code makes a
  missile miss. A hard enough turn is faster than the seeker, the target leaves the
  cone, and the missile goes past.
- A proximity fuse fires the warhead at the closest point. When the missile is inside
  fuse range and the range starts to grow again, that was the closest point.

Turn rate falls as speed rises. So you slow down to escape a missile. This is why the
throttle is a control and not a setting. The enemy has the same limit, so an enemy
that turns away from your missile is in the same trouble you are.

Enemy behaviour has two states:

- **Pursue.** Turn toward the player, close the range, and fire when the player is
  inside a cone ahead of the nose. Fly past and turn back if the range gets too short
  to turn in.
- **Evade.** Turn across the path of an incoming missile, not away from it. A turn
  across a seeker defeats it. A turn away lets the missile catch up.

The numbers are in `cfg_combat.h` and `cfg_flight.h`.

### The four ships

| ship | tagline |
|---|---|
| **AEGIS** | NO WEAKNESS, NO EDGE |
| **LANCE** | CLEAN HITS ONLY |
| **CHARIOT** | FAST, LOUD, FRAGILE |
| **BALLISTA** | KILL THEM FIRST |

The ships differ in hull strength, speed range, magazine size, turn rate and `shake`.
`shake` moves more than the camera. The canopy swings by the same value. So a
CHARIOT canopy moves more than a BALLISTA canopy with the same drawing.

### The tournament

Sixteen pilots, four rounds, and a loss ends the run. You get the first seed. The game
makes the other fifteen with a callsign, a ship, a trail colour, a voice and a rating.
It decides the matches you do not fly from the ratings.

The last opponent gets the second seed. The second seed is in the other half of the
bracket, so that pilot reaches the final. `sim_match` also stops that pilot losing any
match you do not watch. The opening story is about a pilot nobody has beaten, and the
bracket makes sure you meet that pilot.

You get a purse for each round and you spend it on repairs. Damage stays on the ship
between rounds. The hull you fly in the final is the hull you had after the first
round, less the repairs you could afford. Credits start at 0 and stop at 1500. See
`cfg_econ.h`.

Win the tournament and the name is yours. Some pilots start to say so, and the title
screen changes.

Lose it and the name goes to somebody else. If you die while you hold the title, the
game clears your bank, your callsign, your ship and the title. A dead pilot cannot
keep the name. The pilot who killed you takes it: that callsign gets the second seed
in the next tournament and reaches the final. This repeats each time the title
changes.

### The ring course

A test of the controls before the fighting starts. Fly through five rings, one after
another, without a miss. It is here because the first thing to learn is what the stick
does, and a duel is a poor place to learn it. The broadcast explains the test.

### The canopy

There is no crosshair. The canopy is a drawing. `tools/canopy_bake.py` turns a PNG
into a table of runs, and the band renderer applies the table to the finished picture
as a change. So the canopy lights what is behind it. It does not paint over it.
The blend runs on the vector unit of the processor. See **Speed**.

The canopy bends, and it bends the opposite way to the instruments. It bends most when
the throttle is shut and flattens as the ship speeds up. The canopy also swings behind
the ship on three axes. A spring and a damper control the swing, and the ship's
`shake` scales it.

The canopy also arrives one piece at a time. A match starts with a black screen. Each
region of the canopy flashes white. The world then appears through the white as a
dither pattern. The parts of the canopy get bright and then fall to the level you
drew. The order is yours. The same PNG holds the canopy in the **red** channel and a mask in
the **green** channel. The grey value of each shape in the mask is its place in the
order.

The instruments come on next. The radio opens last. Each step waits for the step
before it. See `CANOPY_INTRO_*` in `cfg_hud.h`.

Each ship can have its own canopy. A ship with no drawing flies with no canopy. There
is no default drawing. To add one, put the PNG in `design/canopy/` and run
`.\tools\canopy.ps1`. The file name selects the ship. The rules for drawing one are in
[tools/README.md](tools/README.md).

### The instruments

The interface follows [AmberConsole](https://github.com/DutchDiederik/AmberConsole).
Its rule is that brightness, inverse video and blink show importance. Colour never
does. So the interface uses one colour at many brightness levels. A missile lock shows
as the brightest level and a blink, not as a different colour. A label shows as a
filled block with dark text.

`VG_PALETTE` selects the colours. `VG_PAL_AMBER` gives amber. Any other value gives
the neon set.

The instruments are drawn on a flat grid and then bent onto a curved surface. See
`HUD_WARP_K`, which is -0.22. The renderer bends them, so no drawing code knows about
the curve. Lines are cut into segments and bent. Filled rectangles become strips of
bent quads. Letters keep their shape, and only their positions move.

Two things stay flat. The steering ring stays flat because it must sit under your
finger. Markers that point at objects stay flat because they must line up with those
objects. Touch input is never bent. Only the drawing bends.

The screen dims one row in three by half. A per-pixel effect is normally too slow, but
this one runs inside the band renderer, under the transfer. Vector art is mostly black. So the loop reads
two pixels at a time as one 32-bit word, and one test skips a pair of black pixels. It
pays only for pixels that are lit.

Halving is important. A general scale of each colour channel cost about 18 operations
for each pixel. It also made a band too slow to fit the transfer once a bright sky
filled the screen. A half is a shift and a mask.

The radar is a half ellipse across the bottom of the screen. It shows a flat circle in
steep perspective. Ahead is the top. Your own left-right line is the flat edge. A
contact behind you sits just under that edge, on the correct side, instead of leaving
the display. Where a contact is behind you is the most useful thing the radar shows.
The radar shows fighters and missiles. It does not show asteroids, which are terrain.

A rear-view window sits in the panel. Hold it like a button. It moves with the panel
but stays flat, because a bent window would bend the picture inside it.

### The arena

Every fight is inside a closed space, so there is somewhere to be. The code has two
shapes. **The game uses only the first one.**

- `ARENA_TORUS` is the inside of a tube that closes into a loop. Every fight uses it.
  The game draws hoops around the tube and rails along it, and only inside a window
  ahead of you and behind you. The hoops that shrink into the distance are what make it
  look like a tunnel.
- `ARENA_SPHERE` is the inside of a large hollow sphere, with lines of longitude and
  latitude. **It works, and nothing selects it.** Every call to `vg_arena_init` asks for
  the torus. A tunnel gives the fight more depth and more motion, because a sphere puts
  every wall at the same distance.

The sphere is kept for a reason. It is the second shape, so it shows that the arena code
is not written for one shape only. A new shape must work for both.

The wall gets brighter and redder as you get close. The alarm beeps faster. The whole
picture takes a red gradient. If you touch the wall, the run ends.

### The sky

There are three kinds. `SKY_NEBULA` is a drifting cloud. `SKY_GALAXY` is a barred
spiral with a warm centre and cooler arms. `SKY_CLUSTER` is knots of faint starlight.
Each venue uses one of the three. The menus make their copy more blue, so the amber
text has a background that does not match it.

The sky is a 32 KB texture put onto a sphere. The renderer works out the exact
position at a grid of points for each band and interpolates between them. Sharp stars
are not in the texture. They stay in the point renderer, where they cost very little.
Soft light goes in the texture and sharp light stays as points.

### Sound

Nothing is sampled. The game makes every sound. `vg_sfx.cpp` holds a table that
describes each sound. `vg_synth.cpp` makes it from square, sine and noise waves, a
two-pole low pass filter, an amplitude modulator, and a delay for each layer.

Flash size is not the reason. There is room for twenty samples. There are three better
reasons. Square waves suit a wireframe game. The synthesizer uses less flash than one
second of recorded audio. And a generated sound can change. The missile alarm rises
as the missile gets closer. A dying pilot's tone is lower than a normal one.

The mixer runs on core 0. Measured, it cost up to 4.3 ms in a busy frame. That time
went to the submit stage, in front of the screen transfer, so it delayed the transfer.
The move was safe for a reason and not by luck: `vg_synth.cpp` reads no game state and
has its own random number generator. If it used the game's generator, the audio would
change the simulation and a replay would no longer match.

### What the pilots say

There are seven voices. Each one has lines for a taunt, a shot, a wound and a death.
The game can give six of them to an opponent. The seventh belongs to the last
opponent and is never given to anybody else. There is one PHANTOM, and it is either
the pilot in the final or it is you.

A pilot gets a voice when the bracket is made, so that pilot sounds the same for the
whole tournament. After you win a tournament, some opponents say so.

---

## Speed

This is where most of the work went, so it gets the most space.

### There is no world space

The ship is always at the origin and always looks along +Z. Flying rotates and moves
the world instead of the ship. So the length of a point is its distance from you, and
the projection is two multiplies and a divide. There is no matrix.

### Two stages

**Submit.** The renderer works out where each object is on the screen and adds it to a
flat list. Each call clips against the whole screen. Nothing is drawn yet.

**Flush.** For each of the 15 bands: clear a band-sized buffer in internal RAM, draw
every object that touches that band into it, and give the buffer to DMA.

The reason for two stages is that the band buffer is never in PSRAM. Drawing a line
writes to scattered addresses. Against PSRAM that misses the cache often enough to use
most of the frame. Internal RAM runs at bus speed instead. That one measurement is why
the memory limits below matter.

### Both cores, three ways

| work | how it splits |
|---|---|
| submit | the world on core 1 and the instruments on core 0, at the same time |
| inside a band | the sky, the dim rows and the canopy split by row across both cores |
| audio | the mixer is on core 0 only |

Submit writes into four parts of one list. The flush joins them in order, so the part
a core writes to decides where its objects land in the drawing order. The joined list
is the same, byte for byte, as a list built by one core. This is what lets two cores
build one list without the band renderer knowing.

The canopy splits each band at a point the baker works out from the drawing. A band
costs as much as its slower half, so an even split of uneven work saves little.
Measured, an even split returned 1.2 ms of the 1.9 ms it should have.

### The transfer sets the limit

460,800 bytes at 80 MHz on four data lines takes **11.52 ms**. Every frame pays it and
nothing can change it. The rest of the design hides CPU work inside that time.

The driver sets the screen window once for each frame. The 15 bands then write into it
with a continue command. Setting the window for each band cost about 0.5 ms in driver
work, for a few bits of data.

The driver also keeps one band waiting behind the band that is transferring. The SPI
hardware starts the next band as soon as the current one ends, with no CPU step
between. This needs three band buffers: two are in use and the renderer draws into the
third.

The result was larger than expected. Two bands are still transferring when the flush
loop ends, so about 1.5 ms of transfer now happens during the next frame's input and
submit. The bus is never idle between frames.

This is why `blit` now reads as less than 11.52 ms. It is not a fault. Every byte
still goes out. Some of the transfer is outside the counter that measures it.

### Where the time goes

These are measured in flight. They are not calculated.

| state | frames a second |
|---|---|
| idle loop | 71 |
| a fight | 60 to 61 |
| the ring course | 60 to 64 |
| close to the wall | 57 |

    the course, in flight:  in 810   upd 556   sub 2477   blit 12242
                                              = rast 11555  push 474

`push` is the CPU waiting for DMA. The idle loop and a fight do not agree, and an
average of the two hides it:

| state | rast | push | bands over their limit |
|---|---|---|---|
| idle | 5.6 ms | 6.8 ms | 0 |
| fight | 9.7 ms | 2.0 ms | 2 |

The fight row is from August 2026, after the canopy blend moved to the vector
unit. The change cut `rast` and increased `push` by the same amount. The
processor finishes its drawing sooner, so it waits longer for the transfer.

When idle there is 6.8 ms spare and drawing work is nearly free. In a fight there is
1.6 ms spare and some bands take longer than the 768 us each band gets. Then drawing
work costs frame time directly. Both rows are true. Which one applies depends on what
is on the screen.

Input, update and submit all run before the transfer starts, one after another. A
microsecond saved there is a microsecond off the frame. That is where the recent
savings came from.

### Three memory limits, and one is tight

| memory | free | what goes there |
|---|---|---|
| internal RAM | about 10 KB | the object list, the band buffers, the sky texture |
| PSRAM | 8.1 MB | anything read in order, or read once for each frame |
| flash | 2.6 MB | dialogue, ship tables, models, canopy drawings |

Internal RAM is the only tight one, and the renderer uses all of it. It has to: those
three are written and read at scattered addresses every frame.

Nothing you add to the content goes there. Dialogue and ship tables are `const` and
sit in flash. One pilot voice is about 400 bytes. New ship behaviour costs CPU time in
`upd`, which is about 760 us now. It does not cost memory.

Free memory is not the number to watch. The sky texture needs 32 KB in one piece. A
third band buffer once left 41 KB free but no piece larger than 21 KB. The request
failed and the sky turned off, while the total said there was room. So the sky now
takes its memory before the renderer does. The one request with a fixed minimum piece
size gets it from a heap that is not yet broken up.

### Hidden-line drawing

The game fills the faces of a model in the background colour, then draws the edges
over them. So a hull hides its own back edges and whatever is behind it, and still
looks like line art. Two passes, the same shapes, and no depth buffer.

### Line quality

Lines are anti-aliased by Wu's method. See `VG_LINE_AA`. Each step lights the two
pixels on each side of the true line, and the brightness comes from the position
between them.

This costs about 6 times a solid pixel. There are twice as many pixels, and each one
must be read before it is written, so the loop cannot only write.

So the game pays for it on the instruments, the hulls and the arena, and turns it off
for the whole world layer. The instruments carry nearly everything the player reads. A
ship in a dogfight is small, distant and moving, so a smooth edge on it is worth
little. The cost of a smooth line rises with its length, and the longest lines in a
fight are near a hull.

### The vector unit

The processor has a 128-bit vector unit, and the canopy blend runs on it. One
instruction blends eight pixels. The scalar blend cost 36 cycles for each pixel.
The vector blend costs 5 to 10, and a fight frame gained about 0.7 ms.

The two blends give the same pixels. A test proves this at each start, before
the first frame. The test runs both blends over every source value and compares
the output. If one pixel differs, the test prints it. To use the scalar blend
only, set `CANOPY_PIE` to 0 in `vg_canopy_draw.cpp`.

The unit has limits, and two of them shaped the code. It cannot look up a table
for eight pixels at once, so a blend that uses a table keeps a scalar step. A
saturating add protects a 16-bit lane, not the three colour fields inside it.
So the blend separates each pixel into its three fields, blends each field, and
packs them again. The separated arithmetic gives the same result as the packed
arithmetic, so the test can compare for equal pixels.

### Four ways a measurement can be wrong

Each of these happened during this work. They are the most useful part of it.

1. **A part used as the whole.** Submit is the slower of two halves that run at the
   same time. Only the total of each half says where work should go. One function
   inside a half was timed and used as the half. That moved 570 us onto the core that
   was already busier.
2. **Two different scenes compared.** A measurement with two ships and a measurement
   with one ship are not a before and an after. Each bench fixes what it draws. The
   telemetry line does not.
3. **Two states averaged.** The idle loop and a fight are limited by opposite halves.
   The average of the two gives a difference of 79 us and looks balanced. It is
   neither.
4. **A bench that measures work the game never does.** A checksum caught a false
   number three times. One was a 25% saving that did not exist.

Every bench here reports a checksum for this reason.

---

## Tools

The tools are Python and live in `tools/`. They use the same USB serial port as the
telemetry. See [tools/README.md](tools/README.md).

| tool | what it does |
|---|---|
| `phantom_recorder.py` | record and render video, in a window |
| `phantom_session.py` | the same, from the command line |
| `canopy_set.py` | bake `design/canopy/` and write the table the game reads |
| `canopy.ps1` | bake, build, write to the board, and measure |
| `canopy_cost.py` | ask the board what a canopy costs to draw |
| `replay_cost.py` | play a recorded session and report the cost of each stage |
| `phantom_regress.py` | play a recorded session and compare frame hashes against a baseline |

Recording has two steps because of the serial port. One frame is 460,800 bytes and the
port carries 0.74 MB/s, so the board cannot send 60 frames a second. The limit is about
23.

But a session is one frame time and one input structure for each frame, which is less
than 100 bytes. So the record step saves only those and the game keeps full speed. The
render step then plays the session on the board and reads the pixels at the speed of
the port. The video is 60 frames a second because the game made the frames 1/60 s
apart.

The same replay is the test for changes. One session renders the same frames every
time. A checksum for each band then shows when a change altered a pixel it must
not alter.

Single bytes sent to the board run the benches. They measure what the canopy costs,
what the sky costs, the letter renderer and the line renderer. Each bench runs the old
code and the new code over the same work and compares a checksum.

---

## Where the numbers are

The settings are in `src/vg/cfg_*.h`, in separate files by subject. So a change to
missile balance does not mean reading past the colours.

| file | contents |
|---|---|
| `cfg_display.h` | screen, orientation, bands, projection, dim rows, instrument curve |
| `cfg_palette.h` | every colour, and the byte order they use |
| `cfg_flight.h` | speed, steering, turn rate, hull strength, damage |
| `cfg_combat.h` | missiles, player weapons, enemy behaviour |
| `cfg_world.h` | arena, asteroids, stars, motes, spawning, culling |
| `cfg_hud.h` | instrument layout, radar shape, touch areas, the canopy sequence |
| `cfg_econ.h` | purses, repair prices, the credit limit |

`vg_config.h` includes all of them.

---

## Not done yet

- **More arena shapes.** Each shape needs a `surf`, a `nearest`, an `inward` and a
  `patch_extent` function. The rest of `vg_arena.cpp` works for any shape. The inside
  of a box is the next one to try. It is also the first shape that is not made from
  a formula. So it needs one treatment for each face, not a (u,v) grid.
- **Flat shading** in place of hidden-line drawing. One colour for each face, from the
  angle to a light. It costs the same and looks different. Fill the faces with a lit
  colour in the first pass and remove the second pass.
- **Screen tearing.** The game draws faster than the screen refreshes and does not wait
  for the tear signal. If tearing shows, wait for the signal or limit the frame rate.
- **A gun** for close range, after the missile duel is right.
- **A canopy for the LANCE.** The other three ships have one. The LANCE flies
  without one until somebody draws it.

## Licence

MIT. See [LICENSE](LICENSE).

The one library, [SensorLib](https://github.com/lewisxhe/SensorLib), is also MIT.
Nothing else is copied in. The screen driver, the renderer, the font, the synthesizer
and the sky are written in this repository.
