# PHANTOM

A wireframe dogfight sim for the **Waveshare ESP32-S3-Touch-AMOLED-2.16** — a
480×480 round AMOLED with a touch layer, an IMU and a speaker, driven straight off
the QSPI bus with no widget toolkit in the way.

You fly a tournament of one-on-one missile duels. Sixteen pilots enter, one comes
out, and the legend waiting in the final is the reason the story exists.

Everything on screen is generated: the ships, the arena, the nebula behind it, the
cockpit frame, and every sound. Nothing is sampled and nothing is a bitmap.

- **~22,000 lines**, no vendored code except one MIT sensor library
- **60–61 fps in a fight**, 71 idle, against an 11.52 ms hard floor of wire time
- Runs the game, records itself, and replays a session frame-for-frame

---

## Getting it onto the hardware

You need [PlatformIO](https://platformio.org/). The board profile, the toolchain and
the one dependency all come down on the first build.

```
pio run                 build
pio run -t upload       build and flash
pio device monitor       watch the telemetry
```

The port is picked up automatically. If you have more than one board attached, name
it: `pio run -t upload --upload-port COM6`.

### Two things that will bite you

**Set `PYTHONIOENCODING=utf-8` before uploading.** Without it the upload can hang
forever partway through, and a half-written flash is a board that will not boot.

```
set PYTHONIOENCODING=utf-8         (cmd)
$env:PYTHONIOENCODING = "utf-8"    (PowerShell)
```

**Never interrupt a flash.** `esptool` verifies the hash at the end; a killed upload
leaves the part in a state you have to recover with a full erase.

### Sharing the board with other firmware

This is a dev board and yours may be running something else. Flashing **overwrites
whatever is there** — there is no dual-boot and no partition scheme protecting the
other image.

If another program holds the serial port — a monitor, a tray daemon, an IDE — the
upload fails with `Access is denied` or `could not open port`. Close it first. Only
one process can own the port, and that includes the telemetry monitor.

### What the board actually is

| part | what it does | driver |
|---|---|---|
| CO5300 | 480×480 AMOLED, QSPI at 80 MHz | `vg_port_co5300.cpp`, written here |
| CST9220 | capacitive touch, up to 5 contacts | SensorLib |
| QMI8658 | accelerometer, 250 Hz | SensorLib |
| ES8311 | mono codec and speaker, I²S | `vg_audio` in the port |
| AXP2101 | power management, and the power key | `vg_pmu_*` in the port |

The panel driver is written from scratch because the whole frame budget is the
transfer, and a general-purpose display library cannot hand a band straight to DMA
and return. See **Optimisation** below.

---

## The systems

### Controls

| input | action |
|---|---|
| left edge strip | throttle, under the left thumb. Snaps to the finger. |
| anywhere right of it | steering. Press to set an origin, hold displaced to turn, release to self-centre. |
| quick tap | launch a missile at the locked target |
| second contact | also fires, so you can shoot without giving up the steering finger |
| hold the **+** key | the steering swipe rolls instead of turning |
| hold the rear-view patch | look aft. A look, not a control: it changes nothing about the ship. |
| **PWR** | pause, and the menus |

Steering is **pointer-style**, like an FPS look control: the nose goes where the
finger goes, so dragging toward the top-right aims up and right. `STEER_PITCH_SIGN`
in `cfg_flight.h` inverts it.

It is a **virtual joystick, not tilt.** Tilt was built first and rejected on
hardware: the board gets held at whatever angle is comfortable, and while the code
does calibrate a neutral pose per run, any change of grip breaks it. The accelerometer
path still exists — `STEER_MODE` 2 for tilt, 1 for trackball-style relative steering.

**Roll needs its own key** because there is otherwise no way to roll at all, and roll
is what makes a turn unpredictable rather than merely fast. The same key reports a
menu edge elsewhere: it means two things in two places, and which one applies is the
game's decision rather than the input layer's.

### Flight and combat

Everything interesting falls out of **one** rule: *a missile can only pull so many
degrees per second.*

- Missiles fly **lead pursuit**, aiming where the target will be. That is what
  bends the path into the arc you see.
- The seeker has a cone. If the bearing leaves it, the lock breaks **permanently**
  and the round coasts ballistic. Nothing scripts a miss — a hard enough break
  outruns the seeker's turn rate and it sails past. That moment is emergent.
- A **proximity fuse** detonates at closest approach: once inside fuse range and the
  range starts opening again, that was as close as it was going to get.

**Turn rate scales inversely with speed**, so evading is a *decision to slow down*.
That is the whole reason the throttle is a control and not a setting, and the enemy
gets the same trade — their evasive break is a real duel, not a formality.

Enemy AI has two states. **Pursue**: turn toward the player, close, fire inside a
nose cone, blow through and re-merge if it gets too close to manoeuvre. **Evade**:
break *perpendicular* to an incoming missile, because turning across a seeker
defeats it where turning away just gets you run down.

Tuning lives in `cfg_combat.h` and `cfg_flight.h`.

### Four airframes

| | tagline |
|---|---|
| **AEGIS** | NO WEAKNESS, NO EDGE |
| **LANCE** | CLEAN HITS ONLY |
| **CHARIOT** | FAST, LOUD, FRAGILE |
| **BALLISTA** | KILL THEM FIRST |

They differ in hull, speed range, magazine, agility and `shake` — and `shake` does
more than rattle the camera: the cockpit frame's inertia rides it, so a CHARIOT's
canopy is visibly looser than a BALLISTA's on the same drawing.

### The tournament

Sixteen entrants, four rounds, single elimination. You take the first seed; the rest
are generated with a callsign, a hull, a trail colour, a voice archetype and a
seeding rating. Matches you do not fly are simulated from those ratings.

**The legend takes the second seed**, which puts it in the opposite half and
therefore in the final — and `sim_match` guarantees it never loses a round you do
not see. The story in the opening crawl is about a pilot nobody has beaten, and the
bracket makes sure you meet them.

Between rounds you spend a purse on repairs. Damage carries: the hull you take into
the final is the hull you brought out of the round of 16, minus whatever you could
not afford to fix. Credits start at 0 and cap at 1500 (`cfg_econ.h`).

**Win it and the name is yours.** Rivals start recognising you, and the title card
changes to say the rumour is about you now.

**Then lose it and it changes hands.** Dying as champion resets the player
completely — bank, callsign, hull, title — because the narrative has no room for a
PHANTOM who is still flying around having been killed. And whoever killed you
inherits the name: their callsign is seeded to the next final, guaranteed to get
there, until somebody beats them. Then the cycle repeats.

### The course

A motor test before the fighting: fly through five consecutive rings. It exists
because the first thing to learn is what the stick does, and a duel is a poor place
to learn it. The broadcast talks you through it.

### The cockpit

**There is no crosshair.** The cockpit frame is a drawing — an artist's PNG, baked
to a table of runs and applied by the band rasteriser as a *change* to the finished
picture, so the frame lights what is behind it instead of painting over it.

It flexes, **inverted against the instruments**: full bulge with the throttle shut,
flattening as the ship accelerates, so opening up reads as being pressed back into
the seat rather than pulled toward the glass. It also trails the ship on three axes
through a spring-damper scaled by the airframe.

**And it comes online.** A match opens black. The frame arrives a region at a time:
each region flashes white, the world dissolves out of that white as an ordered
dither, and the members run hot and cool to their drawn level. The order is the
artist's — the same PNG carries the frame in its **red** channel and an activation
mask in its **green**, where a shape's grey value is its place in the sequence.

Then the instruments catch, and only then does the radio open. Each stage waits for
the one before it (`CANOPY_INTRO_*` in `cfg_hud.h`).

**Every hull can have its own, and a hull with no drawing flies with no frame** —
there is no default texture. Drop `design/canopy/ballista.png` in and run
`.\tools\canopy.ps1`; the file name is the wiring. Authoring rules are in
[tools/README.md](tools/README.md).

### The instruments

The HUD follows [AmberConsole](https://github.com/DutchDiederik/AmberConsole).
Its governing rule is that **hierarchy is brightness, inverse video and blink —
never hue**, so the interface is a single-hue intensity ramp: a missile lock reads
as maximum brightness plus blink rather than a colour change, and labels read by
inversion. `VG_PALETTE` selects the ramp: `VG_PAL_AMBER` is CRT phosphor, and
anything else takes the neon branch.

**Spherical warp.** Instruments are laid out on a flat grid and then bent onto a
virtual canopy (`HUD_WARP_K`, −0.22, barrel), applied inside the rasteriser so the
whole panel curves at once without any drawing code knowing. Lines subdivide and
bend, filled rectangles become strips of warped quads, glyphs keep their shape while
their origins follow the curve.

Two things deliberately stay flat: the steering ring, which must sit under your
finger, and world-space markers, which must line up with what they point at. **Touch
handling is entirely unwarped** — only the drawing bends.

**CRT scanlines** halve one row in three. Normally too expensive
per-pixel, but it lands in the per-band rasteriser under DMA cover, and vector art
is overwhelmingly black — so the loop walks two pixels at a time as a `uint32_t` and
skips empty pairs in a single test, paying only for lit pixels. Halving specifically,
because it is a shift and a mask: the per-channel scale it replaced cost ~18 ops a
pixel and broke the DMA window once a lit backdrop reached every pixel.

**Radar** is a half-ellipse dome across the bottom: a circular plan view in steep
perspective. Forward is the top, the flat chord is your own 3–9 line. Contacts
behind you park just under the chord on the correct side rather than vanishing,
because *"he is on my six"* is the most useful thing a radar can say. Bogeys only —
asteroids are terrain.

**A rear-view patch** sits in the panel as a repeater, held down like a button. It
rides the panel's warp rigidly rather than bending, because bending a viewport would
bend the picture inside it.

### The arena

The fight happens inside a closed world, so there is somewhere to *be* rather than
an empty void. Two shapes:

- **`ARENA_TORUS`** — inside the tube of a doughnut: a closed-loop tunnel. Hoops
  around the tube and rails along it, drawn only in a window ahead of and behind
  you, because the receding hoops are what sell it as a corridor.
- **`ARENA_SPHERE`** — inside a big hollow sphere, with meridians and parallels.

Approach the boundary and it brightens and reddens, the annunciator beeps faster,
and the whole frame takes a red gradient. Touch it and the run is over.

### The backdrop

Three procedural kinds — `SKY_NEBULA` (drifting fBm cloud), `SKY_GALAXY` (barred
spiral, warm core and cooler arms), `SKY_CLUSTER` (knots of unresolved starlight).
A venue rolls one. The menus cool their copy toward blue so the amber system text
has something to sit against.

It is a 32 KB texture sampled onto a sphere, evaluated exactly at a grid of points
per band and interpolated between them. Sharp stars stay in the vector point
renderer, where they are nearly free — soft goes in the texture, sharp stays vector.

### Sound

**Nothing is sampled.** Every cue is generated: a table in `vg_sfx.cpp` describing
what each sound *is*, and a synth in `vg_synth.cpp` that renders it — square, sine
and noise through a two-pole low pass, with an amplitude modulator and a per-layer
delay for sequences.

Not a compromise forced by flash; there is room for twenty samples. A wireframe
dogfight scored by square waves is coherent in a way a recording would not be, the
whole synth costs less flash than one second of PCM, and a generated cue can
respond: the missile alert climbs with range, and a dying pilot's blip is pitched
lower than a routine one.

It mixes on **core 0**. Measured, mixing cost up to 4.3 ms in a busy frame and
billed to the submit phase in front of the panel flush, delaying the whole transfer.
Moving it was safe by construction rather than luck: `vg_synth.cpp` touches no game
state and keeps its own RNG, because drawing from the game's stream would make audio
a term in the simulation and take replay determinism with it.

### Pilots talk

Seven voice archetypes, each with lines for taunting, firing, being hurt and dying --
six a rival may be given, plus the legend's own, which is never rolled. There is only
ever one PHANTOM, and it is either the pilot waiting in the final or it is you.

A rival is assigned one at bracket time, so a pilot's personality is consistent
across a tournament. Once you are champion some of them start saying so.

---

## Optimisation

This is the part of the project with the most work in it, so it gets the most space.

### Everything lives in view space

There is no world-space camera. The ship is always at the origin looking down +Z,
and flying rotates and translates *the universe* instead. So a view-space point's
length **is** its distance from you, and the projection is two multiplies and a
divide with no matrix anywhere.

### Two-stage rasteriser

**Submit**: the renderer projects the scene and appends screen-space primitives to a
flat list. Each call clips against the full screen. Nothing is drawn.

**Flush**: for each of 15 horizontal bands, clear a band-sized buffer in **internal
SRAM**, draw every primitive overlapping that band into it, and hand it to DMA.

The point of the split is that the band buffer never lives in PSRAM. Line
rasterisation is a scattered write pattern, and doing it against PSRAM thrashes the
cache badly enough to dominate the frame. Internal SRAM buys the whole frame at
internal-bus speed instead — and that one finding is why the memory budget below
matters so much.

### Both cores, three ways

| what | how |
|---|---|
| submit | the world on core 1, the instruments on core 0, concurrently |
| within a band | the backdrop, the scanlines and the cockpit split by row across both cores |
| audio | the mixer lives on core 0 entirely |

Submit splits into **four slices** of one primitive list, joined in index order at
flush — so a core's slice decides where its work lands in draw order, and the join
produces byte-for-byte the array a serial submit would have. That is what lets two
cores build one list without the band raster knowing.

The cockpit's row split uses a **baked per-band balance point** from the drawing,
because a band costs whichever half is slower: an even-looking split of uneven work
returned 1.2 of the 1.9 ms it should have.

### The wire is the floor

460,800 bytes at 80 MHz quad-SPI is **11.52 ms**, every frame, and nothing can move
it. The whole architecture is arranged around hiding CPU work underneath it.

One address window is set per frame and the fifteen bands stream into it as
memory-continue writes; re-windowing per band cost ~0.5 ms in driver overhead for a
few bits on the wire.

**A band is kept queued behind the one in flight**, so the SPI engine starts the
next the instant the current one ends with no CPU in the path. That needs three band
buffers: two outstanding, one to rasterise into. The effect was larger than
predicted — two bands are still transferring when the flush loop exits, so ~1.5 ms
of transfer now overlaps the *next* frame's input and submit, and the wire never
idles at the frame boundary.

Which is why **`blit` reads below the wire floor and that is not a fault.** Every
byte still goes out; some of it is outside the bracket that measures it.

### Where the frame goes

Flown, not modelled:

| | frame rate |
|---|---|
| attract loop | 71 |
| a fight | 60–61 |
| encroaching on the boundary | 57 |

```
a fight:   in 720   upd 758   sub 2851   blit 12252
                                       = rast 10461  push 1598
```

`push` is the CPU standing idle on DMA. **The two regimes disagree, and averaging
across them hides it completely:**

```
attract   rast  5.6 ms   push 6.8 ms   0 bands over their window
fight     rast 10.5 ms   push 1.6 ms   3.5 bands over
```

Idle, there is 6.8 ms of slack and raster work is nearly free. In a fight there is
1.6 ms and bands overrun the 768 µs each gets, so raster work costs frame time
directly. Both are true; which applies depends on the scene.

**The pre-flush phase is serial**, so a microsecond cut from input, update or submit
comes straight off the frame. That is where the recent wins came from.

### Three memory budgets, and only one is tight

| pool | free | what belongs there |
|---|---|---|
| internal SRAM | ~10 KB | the primitive list, the band buffers, the backdrop texture |
| PSRAM | 8.1 MB | anything read sequentially or once per frame |
| flash | 2.6 MB | dialogue, ship tables, models, canopy drawings |

**Internal SRAM is the only pressed one and it is entirely the renderer's.** It has
to be — those three are written and sampled in scattered patterns every frame.

Nothing content adds goes there. Dialogue and ship tables are `const` and live in
flash; a pilot archetype is about 400 bytes. What behaviour work costs is CPU in
`upd`, currently ~760 µs, not memory.

**Total free is not the number.** The backdrop needs 32 KB *contiguous*, and a third
band buffer once left 41 KB free with a largest block of 21 — so the allocation
failed and the nebula switched off entirely while the sum said there was room. The
backdrop therefore claims its texture *before* the rasteriser, so the one allocation
with a hard contiguous minimum takes it from an unfragmented heap.

### Hidden-line rendering

A model's faces are filled in the background colour and its edges drawn over them,
so a hull occludes both its own back edges and whatever is behind it, while still
reading as vector art. Two passes, same geometry, no depth buffer.

### Line quality

Lines are **Wu antialiased** (`VG_LINE_AA`), which cost about 6x a solid Bresenham
pixel: twice the pixels, and unlike a plain store it must READ the destination to
blend, so it cannot be a write-only loop. So it is paid for instruments,
hulls and arena structure — and switched **off for the whole world layer**, because
the HUD carries nearly everything the player needs to read while a dogfight gives
the ship itself very little screen. An AA span costs per pixel, so its price tracks
length, and in combat the long spans are near ship hulls.

### Four ways a measurement lies

Hard-won, and the transferable part of all of this:

1. **Balancing against a part instead of the whole.** Submit is the *slower* of two
   concurrent halves, so only each half's total can decide where work goes. Timing
   one function inside a half and treating it as the half sent 570 µs of work onto
   the core that was already busier.
2. **Comparing across scenes.** A before with two ships and an after with one is not
   a before and after. Benches pin their workload; the telemetry line does not.
3. **Averaging across regimes.** Idle and combat gate on opposite halves. Averaged,
   the gap reads as 79 µs and "nearly balanced" — it is neither.
4. **A bench measuring work the program does not do.** Three separate times a
   checksum was the only thing that caught a false number, including a 25% win that
   did not exist.

Every bench here reports a checksum for exactly that reason.

---

## Tools

Python, in `tools/`, talking to the board over the same USB serial the telemetry
uses. Full documentation in [tools/README.md](tools/README.md).

| | |
|---|---|
| `phantom_recorder.py` | record and render footage, in a window |
| `phantom_session.py` | the same from the command line |
| `canopy_set.py` | bake `design/canopy/` and generate the wiring |
| `canopy.ps1` | bake, build, flash and measure in one command |
| `canopy_cost.py` | ask the board what a canopy costs to draw |

**Recording is two steps, because of the link.** One frame is 460,800 bytes and the
link carries 0.74 MB/s, so the device cannot send 60 frames a second — the limit is
about 23. But a *session* is one frame time and one input structure per frame, under
100 bytes. So a record saves only those and the game keeps full speed; a render then
replays the session on the device and reads the pixels at the link's pace. The video
is a true 60 fps because the game made the frames 1/60 s apart.

That replay is also the regression harness: the same session renders frame-for-frame
identically, and band checksums say when a refactor changed a pixel it should not
have.

Single-byte serial commands run the benches — what the cockpit costs, what the
backdrop costs, the glyph nest and the line walk, each old against new over an
identical workload with a checksum to keep it honest.

---

## Tuning

Everything is in `src/vg/cfg_*.h`, split by concern so changing missile balance does
not mean scrolling past the palette:

| | |
|---|---|
| `cfg_display.h` | screen, orientation, banding, projection, scanlines, HUD warp |
| `cfg_palette.h` | every colour, and the byte-swap convention behind them |
| `cfg_flight.h` | speed, steering, agility, hull integrity, damage |
| `cfg_combat.h` | missiles, player weapons, enemy behaviour |
| `cfg_world.h` | arena, asteroids, stars, motes, spawning and culling |
| `cfg_hud.h` | HUD layout, radar geometry, touch zones, the cockpit sequence |
| `cfg_econ.h` | credit purses, repair pricing, the bank ceiling |

`vg_config.h` is an umbrella that includes them all.

---

## Not done yet

- **More arena shapes.** `vg_arena.cpp` needs a `surf`, a `nearest`, an `inward` and
  a `patch_extent` per shape; everything else is generic. A box interior is the
  obvious next one, and the first non-parametric shape, so it would want a per-face
  treatment rather than a (u,v) grid.
- **Flat shading** instead of hidden-line: one colour per face from a light dot
  product. Same cost, different look — swap the background fill in pass 1 for a lit
  colour and drop pass 2.
- **Tearing.** We render faster than the panel refreshes and do not sync to TE. If
  tearing shows, either enable it and wait, or cap the frame rate.
- **A gun** for close-in work, once the missile duel feels right.
- **Canopies for three hulls.** The CHARIOT has one; AEGIS, LANCE and BALLISTA fly
  without a frame until somebody draws them.

## License

MIT — see [LICENSE](LICENSE).

The one dependency, [SensorLib](https://github.com/lewisxhe/SensorLib), is MIT as
well. Nothing else is vendored: the panel driver, rasteriser, font, synth and
procedural backdrop are all written from scratch in this repo.
