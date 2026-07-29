# PHANTOM

A wireframe **dogfight sim** for the **Waveshare ESP32-S3-Touch-AMOLED-2.16**
(480x480 CO5300 AMOLED, CST9220 touch, QMI8658 IMU, AXP2101 PMU).

Old-school vector graphics on black at **~70 fps**. You hunt an enemy fighter and
trade heat-seeking missiles with it — throttling constantly to tighten your turn
when something is chasing you and to run when it isn't.

This README describes the **engine**. The game on top of it — a sixteen-entrant
knockout tournament with four ship classes, a repair economy and a roster of
pilots who talk back — is specified in a design document kept out of this
repository.

---

## Build & flash

Requires [PlatformIO](https://platformio.org/). From the repository root,
substituting your serial port:

```
pio run --target upload --upload-port COM6
pio device monitor --port COM6 --baud 115200     # telemetry
```

The only dependency is `lewisxhe/SensorLib` (IMU + touch); the panel is driven
directly and pulls in nothing.

The firmware prints a per-frame cost breakdown over serial, which is how every
performance claim below was measured:

```
66.7 fps | in 871us upd 183us submit 1203us blit 13407us | prims 106
```

### Sharing the board with other firmware

If something else on the host holds the serial port open (a companion daemon,
a monitor), stop it before flashing or the upload will fail with an access
error. On Windows the offender is usually a background `pythonw` process.

---

## Controls

| Input | Action |
|---|---|
| Left edge strip | Throttle, under the left thumb. Snaps to the finger. |
| Anywhere right of it | Steering. Press to set an origin, hold displaced to turn. Self-centres on release. |
| Quick tap | Launch a missile at the locked target. |
| Second finger | Also fires, so you can shoot without giving up the steering finger. |

Steering is **pointer-style**, like an FPS look control: the nose goes where the
finger goes, so dragging toward the top-right aims the ship up and right. Flip
`STEER_PITCH_SIGN` in `vg_config.h` for inverted / classic-stick feel.

It is a **virtual joystick**, not tilt. Tilt was built first and rejected on
hardware: the board gets held at whatever angle is comfortable, and while the
code does calibrate a neutral pose per run, any change of grip breaks it. The
accelerometer path still exists — set `STEER_MODE` to `2`, or `1` for
trackball-style relative steering.

---

## The combat model

Everything interesting falls out of **one** rule: *a missile can only pull so
many degrees per second.*

- Missiles fly **lead pursuit**, aiming where the target will be rather than
  where it is. That is what bends the path into the arc you see.
- The seeker has a **60-degree cone**. If the bearing to the target ever leaves
  that cone the lock breaks **permanently** and the missile coasts ballistic.
  Nothing scripts the miss — a hard enough break simply outruns the seeker's
  turn rate, the bearing slides out of the cone, and it sails past. That moment
  is emergent, not a dice roll.
- A **proximity fuse** detonates at closest approach: once inside fuse range and
  the range starts opening again, that was as close as it was going to get.

### Why the throttle matters

Turn rate scales inversely with speed (`AGILITY_*` in `vg_config.h`):

| throttle | your turn rate | vs. a missile's 2.5 rad/s |
|---|---|---|
| idle | ~3.3 rad/s | **you out-turn it** |
| full | ~1.33 rad/s | you cannot |

So evading is a *decision to slow down*, which is the whole reason the throttle
is a control and not a setting. Enemies get the same trade, so their evasive
break is a real duel rather than a formality.

### Enemy AI

Two states. **Pursue**: turn toward the player, close, fire when the player is
inside a 25-degree nose cone and in range; blow through and re-merge if it gets
too close to manoeuvre. **Evade**: when a player missile gets within
`ENEMY_EVADE_RANGE`, break *perpendicular* to the missile's heading (turning
across a seeker defeats it; turning away just gets you run down) and bleed speed
to tighten the break.

### Arenas

The fight happens inside a closed world, so there is somewhere to *be* rather
than an undifferentiated starfield.

Currently **torus only**: the inside of a doughnut's tube, major radius 1500,
tube radius 420 — a closed-loop tunnel. It gives depth, a sense of place, and a
line to fly along. `ARENA_SPHERE` (a hollow sphere, radius 1500) is still
implemented and selectable, but reads as far less interesting because it is
uniform in every direction; it stays in for when there is a roster of maps worth
cycling through.

Boundaries are **analytic, not meshes**. Every shape is a parametric surface
`point(u,v)` plus a clearance query, so the renderer generates grid lines
wherever and at whatever density it likes without storing any geometry. That is
precisely what makes proximity-adaptive detail cheap: drawing a finer grid just
means evaluating more points.

Structure **fades with distance** (`arena_seg`): the player sits at the origin,
so a view-space point's length is already its distance from the ship. The wall
you are closing on reads brightly, the far side of the world sits faintly at the
edge of perception. That gives depth, guarantees a frame of reference wherever
you look, and acts as a second proximity cue underneath the grid densifying.

Grid spacing is ~20 degrees on the sphere, chosen so several lines always sit
inside the 62-degree FOV. At the original 36-degree spacing you could face a gap
and see nothing at all.

The boundary draws in two passes:

1. **Structure** — a coarse grid so the shape of the world is always legible.
   For the torus that means hoops around the tube spaced by arc length and drawn
   only in a window ahead of and behind you, which is what sells it as a
   corridor; they are snapped to a global grid so they stay put in the world
   instead of sliding along with you.
2. **Proximity patch** — a fine patch tracking the nearest point of the wall
   that *tightens, shrinks and brightens* as you close, going cyan → amber →
   red. This is the real "you are running out of room" cue; the `BOUNDARY`
   text is a last-ditch backup that should never be the first thing you notice.

Patch size is specified in **world units, not angle**, because equal angles
cover wildly different arc lengths near a sphere's pole or on the inner wall of
a torus. `vg_arena_patch_extent()` converts.

Consequences elsewhere:

- The boundary blends toward **red** as you close on it, weighted by each
  segment's own distance — so only the wall you are actually approaching lights
  up, while the far side of the world stays cool. A uniform tint would say
  "danger" without saying "that way".
- Touching the wall costs a life. Since the player is nailed to the origin,
  "bouncing off" means shoving the *arena* the other way — otherwise you would
  still be inside the wall when invulnerability expired and lose every remaining
  life at once.
- Enemy AI treats wall avoidance as its **highest** priority, above evading a
  missile: flying into the boundary to dodge is just a slower death.
- Missiles detonate against the boundary, so leading one into a wall is a
  legitimate way to defeat it.
- Spawns are pushed inside via `vg_arena_clamp_inside()`.

In the torus, flying straight from the centreline puts you into the wall after
about 1100 units — roughly 20 seconds at cruise — so the tunnel has to be
actively flown.

### Procedural backdrop

A nebula, generated once per run into a 128x128 texture and sampled per band.

The renderer is a sparse vector rasteriser; a nebula is the opposite — dense,
area-filling, smoothly varying. The governing number is **~10 cycles per pixel**,
which is the per-band CPU time left over under DMA. Live per-pixel noise is
50–200 cycles and never had a chance; a table lookup with an incremental affine
step is ~3, so the noise is baked once (~235 ms at level start) and the per-frame
cost collapses to a lookup.

Three things make it effectively free:

- The fill **replaces the band memset** rather than adding to it, so only the
  excess counts.
- It writes **two pixels per 32-bit store** from a single sample. At a 3.3x
  upscale the duplicated column is invisible, and it halves both sampling and
  store traffic.
- It lives in the per-band rasteriser, which is hidden under DMA — so it costs
  nothing in `submit`, the stage actually under pressure.

Measured: blit 12.19 → 12.65 ms, ~68 → 67.5 fps.

**Scanlines had to change.** They were cheap only because the screen was ~95%
black and the pass could skip empty pixel pairs; with every pixel lit that
shortcut dies and the pass would cost several ms. So the darkening is baked into
a **second pre-darkened copy** of the texture and selected per row during the
fill — zero extra per-pixel cost. Vector art on top is no longer darkened, which
reads better anyway: bright strokes stay crisp over a striped background, much
as they do on real hardware.

The backdrop is at infinity, so only rotation moves it. A true per-pixel skybox
lookup would need a ray direction per pixel; instead it pans and rolls as a
tiling 2D layer, which over a 62-degree FOV is indistinguishable for something
this amorphous. The lattice wraps, so it tiles seamlessly — it does repeat every
~6 texture widths of yaw, which no one has yet noticed mid-fight.

Sharp detail deliberately stays out of the texture: a low-res image cannot draw
crisp stars, so star clusters remain in the vector point renderer where they are
nearly free. **Soft goes in the texture, sharp stays vector.**

### Hull integrity

A single hull meter, not discrete lives. It self-repairs after
`HEALTH_REGEN_DELAY` seconds clear of combat — but **only while it is above
`HEALTH_REGEN_FLOOR` (30%)**. Drop below that and the damage is permanent, so a
bad fight leaves you crippled for the rest of the run rather than merely dented.

Being *hunted* counts as combat even before anything connects: a tracking
missile within `THREAT_COMBAT_RANGE` holds the repair clock at zero, so you
cannot patch up while a seeker is still chasing you.

The meter fill slides amber → red as integrity falls and blinks below the
repair floor. That is the one place the interface leaves its single hue —
losing the ship is worth breaking the rule for.

### Interface treatment

The HUD follows [AmberConsole](https://github.com/DutchDiederik/AmberConsole),
using its exact design tokens converted to RGB565. Its governing rule is that
**hierarchy is brightness, inverse video and blink — never hue**, so the
interface is a single-hue intensity ramp (`INK_TRACE` … `INK_MAX`): a missile
lock reads as maximum brightness plus blink rather than as a colour change, and
panel labels read by inversion (filled block, dark text). Panels are 2px
strokes, no radius, no shadow. `VG_PALETTE` switches the whole thing between
amber CRT phosphor and neon plasma.

**CRT scanlines** knock one row in three down to ~63%. Normally too expensive
per-pixel, but it lands in the per-band rasteriser, which is hidden under DMA —
and vector art is overwhelmingly black, so the loop walks two pixels at a time
as a `uint32_t` and skips empty pairs in a single test, paying only for lit
pixels.

**Spherical warp.** Instruments are laid out on a flat grid and then bent onto a
virtual canopy, so the HUD reads as part of the aircraft rather than pasted on
in 2D. It is applied inside the rasteriser (`vg_hud_warp`), not at the call
sites, so the entire HUD curves at once without any drawing code knowing about
it: lines subdivide and bend, filled rectangles become strips of warped quads,
and glyphs keep their shape while their origins follow the curve.

Two things deliberately stay flat: the steering ring, which must sit exactly
under your finger, and world-space markers, which must line up with what they
point at. **Touch handling is entirely unwarped** — only the drawing bends.

The warp is barrel (negative `HUD_WARP_K`), which is both the classic CRT face
and the safe direction: pincushion pushes the left-edge throttle off-screen.

### Radar

A half-ellipse dome across the bottom — a circular plan view seen in steep
perspective. Forward is the top of the dome, the flat chord is your own 3-9
line, and range is distance from the centre. Contacts behind you are parked just
under the chord on the correct side rather than vanishing, because "he is on my
six" is the most useful thing a radar can tell you. **Bogeys only** — asteroids
are terrain, not contacts. The locked target shows boxed and in yellow.

---

## Architecture

Everything is in `src/vg/`, prefixed `vg_`, so it can drop into another
firmware's `src/` without colliding.

```
main.cpp                 standalone entry: timing loop + telemetry
vg/
  vg_port.h              >>> THE PORTING SEAM <<<
  vg_port_co5300.cpp     standalone impl: CO5300 QSPI + CST9220 + QMI8658

  vg_config.h            umbrella; tuning lives in the cfg_* files
  cfg_display.h            screen, orientation, banding, scanlines, HUD warp
  cfg_palette.h            colours + the byte-swap convention behind them
  cfg_flight.h             speed, steering, agility, hull, damage
  cfg_combat.h             missiles, player weapons, enemy behaviour
  cfg_world.h              arena, asteroids, stars, motes, spawning
  cfg_hud.h                HUD layout, radar geometry, touch zones

  vg_vec.h               Vec3 / Mat3
  vg_proj.h              projection + its exact inverse

  vg_raster.h            public rasteriser API
  vg_raster_int.h          internal contract between its two halves
  vg_raster.cpp            SUBMIT: warp, rotate, clip, append   (once/frame)
  vg_band.cpp              RASTER: draw a band, blit it         (15x/frame)
  vg_font.h/.cpp           5x7 bitmap font

  vg_sim.h               internal contract between the sim modules
  vg_game.h                public sim API
  vg_game.cpp              state machine, world step, weapons, collisions
  vg_models.cpp            RNG, turn_toward, procedural geometry
  vg_missile.cpp           seeker guidance, proximity fuse
  vg_ai.cpp                enemy fighter behaviour
  vg_arena.h/.cpp          analytic map boundaries + clearance queries
  vg_sky.h/.cpp            procedural backdrop
  vg_input.h/.cpp          contacts (and optionally tilt) -> game intent

  vg_draw.h              shared edge helpers + module entry points
  vg_render.h/.cpp         frame orchestration: draw order and warp bracketing
  vg_draw_world.cpp        stars, motes, rocks, fighters, missiles, wreckage
  vg_draw_arena.cpp        boundary wireframe
  vg_hud.cpp               instruments, radar, lock box, markers
  vg_overlay.cpp           title card, vignette, state text
```

Two of these splits are along **performance** seams rather than subject ones, and
that is deliberate:

- `vg_raster.cpp` vs `vg_band.cpp` -- submit runs once a frame and bills frame
  time directly; the band raster runs 15 times and hides under DMA. They have
  opposite cost characteristics, and having them in one file is exactly why it
  was so easy to keep mis-estimating which change was expensive.
- `vg_draw_arena.cpp` is separate from `vg_draw_world.cpp` because it is
  *generated* rather than drawn from stored geometry -- hundreds of procedural
  segments a frame, all landing in submit.

### Everything lives in view space

The player is permanently at the origin looking down `+z`; the world rotates and
translates around it. One counter-rotation matrix per frame is applied to every
star, ship, missile, trail point and fragment. No camera matrix, coordinates stay
small however far you fly, and "is it in front of me" is a plain `z` test.

Two consequences worth knowing:

- **Enemy missiles need no lead prediction.** In this frame the player really is
  stationary, so pure pursuit toward the origin is already a correct intercept.
- **Missile trails must ride the same transform** as everything else, or they
  smear behind the missile as you manoeuvre.

Enemy orientation is carried as a **forward+up pair**, not a matrix, and
re-orthonormalised every frame — folding the world rotation into a matrix each
frame would slowly accumulate shear.

### Two-stage rasteriser

1. **Submit** — project the scene, append screen-space primitives (line / point
   / rect / glyph / triangle) to a flat list. Each is clipped against the full
   screen once, here, so nothing downstream sees a wild coordinate.
   `ymin`/`ymax` are precomputed so the per-band test is two compares. One flat
   list in submission order is what makes painter ordering trivially correct.
2. **Flush** — for each of 15 horizontal bands, clear a 480x32 buffer, draw
   every primitive overlapping it, queue the DMA, and immediately start
   rasterising the next band into the *other* buffer.

The band buffers are deliberately in **internal SRAM**, not PSRAM. Line
rasterisation is a scattered write pattern and doing it against PSRAM thrashes
the cache badly enough to dominate the frame.

32 rows is chosen precisely: 480x32x2 = 30720 bytes, just under the S3's
32768-byte per-transaction ceiling, so a band is exactly one DMA with no
chunking — and 480/32 = 15 exactly.

### Why the rasteriser is effectively free

Blits are asynchronous (`spi_device_queue_trans`) with two band buffers, so band
N+1 is drawn while band N is on the wire. Before this, the CPU spent 11.5 ms of
every 14.4 ms frame busy-waiting inside `polling_transmit` — 80% of the frame
doing nothing.

That hidden budget is why hidden-line rendering cost **nothing measurable**:
adding ~70 filled triangles per frame moved the blit from 12.15 ms to 12.2 ms,
which is noise. Anything that fits in ~0.7 ms per band is free.

`vg_rast_flush()` waits for the previous frame's *last* band at the top rather
than the bottom, so that final transfer overlaps this frame's input and
simulation, which have already run by then.

### Hidden-line rendering

Asteroids are drawn solid without a depth buffer (480x480x16 bits would be
460 KB — it does not fit, and in PSRAM it would thrash exactly as the
framebuffer would have):

- Faces are recovered at boot as any three mutually-adjacent vertices, wound so
  the normal points outward. No hardcoded table.
- Per frame, a face is front-facing if `dot(normal, A) < 0` — the eye is the
  origin, so the view vector to the face is just its first vertex.
- **Pass 1** fills every front face in the background colour; **pass 2** draws
  those faces' edges over the top. The fills hide both the model's own back
  edges and the stars behind it. All fills must precede all edges, or a
  neighbouring face's fill erases the shared edge just drawn.
- Between rocks, painter order by z. **Within** a rock nothing is sorted: the
  models are kept near-convex (radial jitter only, 0.80–1.22) so front faces
  never overlap on screen and back-face culling alone is correct. That culling
  also halves both the fill and edge work.

Faces straddling the near plane skip their fill rather than doing polygon
clipping — that only happens when a rock is on top of you, which is already a
collision, and the edges still clip properly.

Enemy ships are deliberately still wireframe and drawn *after* the asteroids, so
they are never hidden behind a rock. That is a gameplay call, not an oversight:
losing the bandit behind scenery in a dogfight is worse than the small
inconsistency.

### The panel driver

`vg_port_co5300.cpp` drives the panel directly. Arduino_GFX was dropped after
measurement: its `writePixels()` walks every pixel through a CPU byte-swap into
a bounce buffer, costing ~13 ms/frame on top of the ~11.5 ms the wire needs, and
it exposes no zero-copy path (`write16bitBeRGBBitmapR1` transposes for rotation,
so it copies too).

Instead, **colours are stored pre-byte-swapped** (`VGC()` in `vg_config.h`), so
the rasteriser only ever copies values it never interprets, and a blit is a
straight DMA out of its memory. `vg_dim()` is the one function that interprets a
colour; it swaps in and back out, and runs per object, not per pixel.

Protocol, shared by this whole family of QSPI AMOLEDs:

```
command : cmd 0x02, addr = reg << 8, params on 1 line
pixels  : cmd 0x32, addr = 0x2C00,   data on 4 lines (QIO)
```

Command and address phases stay single-line in both cases. Each transaction is
self-contained, so hardware CS works — no need for the manual CS juggling
Arduino_GFX does to hold a chip-select across chunked writes.

**Two hardware limits worth knowing**, both learned the hard way:

- The S3's SPI length register is 18 bits, so one transaction tops out at
  **32768 bytes**. A 60-row band is 57.6 KB and got rejected with
  `txdata transfer > hardware max supported len`. `BAND_H` is now sized so a
  band is one transaction, and a `static_assert` enforces it.
- The CO5300 only accepts **even-aligned windows**, so band heights and offsets
  must stay even.

Note that the address-window commands are *polled* transactions, and IDF forbids
those while anything is queued — so `push_band` drains the previous transfer
before moving the window. That drain is also what makes the caller's buffer
ping-pong safe.

---

## Measured performance

480x480, 80 MHz QSPI, on hardware:

With the arena grid, hidden-line asteroids and hidden-line ships all on:

| stage | time |
|---|---|
| input (I2C touch) | 0.80 ms |
| game update (flight + AI + missiles) | 0.07 ms |
| scene submit | 0.81 ms |
| **panel blit** | **12.0 ms** |
| | **~73 fps** |

How it got there:

| change | blit | fps |
|---|---|---|
| baseline: Arduino_GFX, 40 MHz, 1024-px chunks | 39.1 ms | 24.7 |
| `ESP32QSPI_MAX_PIXELS_AT_ONCE=7200` | 36.0 ms | 26.8 |
| 80 MHz QSPI | 24.8 ms | 38.9 |
| custom driver, pre-swapped colours | 13.5 ms | 69.5 |
| async DMA + ping-pong bands | 12.15 ms | ~70 |
| + hidden-line asteroids | 12.2 ms | ~70 |
| + arena boundary grid | 12.0 ms | 69 |
| + incremental trig on grid lines | 12.0 ms | 73 |
| + CRT scanlines, warped HUD, speed motes | 12.2 ms | 68 |
| **+ procedural nebula backdrop** | **12.65 ms** | **67.5** |

12.2 ms against a hard floor of 11.52 ms of wire time (460,800 bytes at 80 MHz
quad). The remaining ~0.7 ms is the first band's rasterisation plus 30 small
window-command transactions. **This is done** — the panel is saturated.

### Line quality

Lines are **Wu anti-aliased** (`VG_LINE_AA`): each step lights the two pixels
straddling the true line with coverage from its fractional position. That matters
more here than it would elsewhere, because the spherical HUD warp turns every
straight instrument line into a shallow arc, and shallow diagonals are the worst
case for stair-stepping.

It costs roughly 6x a solid Bresenham pixel -- twice the pixels, and unlike a
plain store it must READ the destination to blend, so it cannot be a write-only
loop. Measured at 0.5-1.4 ms, i.e. free on a sparse screen and about 5 fps in a
heavy fight. Two things keep it that cheap: the source colour leaves panel byte
order once per LINE rather than per pixel, and the blend splits R+B from G so all
three channels ride two multiplies with no field borrowing into the next.

Most of that was paid for by adding a **3D trivial reject** before projection
(`vg_cull_code`). The arena grid and missile trails generate far more segments
than ever land on screen, and each one used to cost two divides, a screen clip, a
`sqrt` and two colour blends before anything noticed. The bound uses the screen's
*circumscribed* radius so the test stays valid under bank -- a rolled camera
rotates the frustum, and a tight rectangular bound would clip visible geometry.
Submit went 1.6 -> 1.3 ms.

### Two lessons

**Hidden-line rendering was free** — rasterisation
hides under DMA, so anything moved into the per-band raster costs nothing.
**The arena was not**, because grid generation happens in `submit`, which runs
before the flush and is now the only stage that costs frame time.

The arena's first cut spent 1.5 ms in submit; almost all of it was `sinf`/`cosf`,
since evaluating a surface point per grid vertex meant 2 trig calls per segment.
Walking each polyline with an incremental rotation instead (4 trig calls per
*line*) took submit to 0.81 ms and bought 4 fps — more than the entire arena
feature had cost.

So: work added to the band raster is free; work added to submit is not.

Further gains would need a higher QSPI clock (untested above 80 MHz) or fewer
bytes on the wire (dirty-rectangle tracking, which a rotating starfield defeats).

---

## Embedding in another firmware

The game is designed to be launched from inside an existing application rather
than only run standalone. `vg_port.h` is the only file that touches hardware,
and everything under `src/vg/` is prefixed `vg_` so it can be dropped into
another `src/` without colliding.

1. Copy `src/vg/` into the host firmware's `src/`.
2. **Drop `vg_port_co5300.cpp`** and add your own implementation of the same
   functions on top of whatever display and touch layer the host already has —
   or keep this file, since it is self-contained and likely faster than routing
   through a general-purpose graphics library.
3. Have the launcher call, per frame:
   `vg_input_update()` -> `vg_game_update()` -> `vg_render_frame()` -> `vg_rast_flush()`.

No other game file changes.

Four things to watch, learned from planning exactly this against an LVGL
application on the same board:

- **Bus ownership.** If you keep this QSPI driver, it and the host's own display
  driver will both want SPI2 and the same pins. Only one may hold the bus, so
  entering the game has to tear down the other — or borrow its
  `spi_device_handle_t`.
- **The host's UI toolkit owns the panel.** The game bypasses it and writes bands
  directly, so on entry it must stop that toolkit flushing, and on exit force a
  full repaint (`lv_obj_invalidate(lv_screen_active())` under LVGL).
- **Rotation.** If the host applies its own software rotation, the game will
  fight it — it does its own quarter turn at submit time. Lock the host's
  rotation while playing, or accept an extra full-frame copy.
- **Multi-touch.** Many touch HALs return a single point. The game needs up to
  five: the throttle can be held while steering while a button is pressed.

---

## Tuning

All in `vg_config.h`. The ones that actually change how it plays:

- **The missile duel** — `MISSILE_TURN_RATE` (2.5 rad/s) and
  `MISSILE_SEEKER_COS` (cos 60 deg). Raise either and missiles become
  undodgeable; lower them and they are decoration.
- **The throttle trade** — `AGILITY_SLOW_BONUS`, `AGILITY_FAST_MALUS`,
  `TURN_RATE`.
- **Feel** — `STEER_RANGE`, `STEER_DEADZONE`, `STEER_LERP`, `STEER_PITCH_SIGN`.
- **Enemy** — `ENEMY_TURN_RATE`, `ENEMY_FIRE_GAP`, `ENEMY_EVADE_RANGE`,
  `ENEMY_HP`.
- **Rearming** — `PLAYER_MISSILES` (6), `PLAYER_RELOAD` (3.2 s),
  `PLAYER_LOCK_TIME`.
- **Radar** — `RADAR_RX/RY/CY`, `RADAR_RANGE`.
- **Budget** — `MAX_PRIMS` (2600; watch for `OVERFLOW` in the telemetry line).

---

## Not done yet

- **Sound.** The board has an ES8311 codec and a speaker on I2S, entirely unused
  so far. A launch whoosh and a lock tone would add a lot.
- **More maps.** `vg_arena.cpp` needs a `surf`, a `nearest`, an `inward` and a
  `patch_extent` per shape; everything else is generic. A box/hangar interior is
  the obvious next one, though it is the first non-parametric shape so it would
  want a per-face treatment rather than a (u,v) grid.
- **Flat shading** instead of hidden-line: one colour per face from a light dot
  product. Same cost, different look — swap `COL_BLACK` in pass 1 for a lit
  colour and drop pass 2.
- **Tearing.** We render faster than the panel refreshes and do not sync to TE
  (`0x35`). If tearing shows, either enable TE and wait on it, or cap the frame
  rate.
- **A gun** for close-in work, once the missile duel feels right.

## License

MIT — see [LICENSE](LICENSE).

The one dependency, [SensorLib](https://github.com/lewisxhe/SensorLib), is MIT
as well. Nothing else is vendored: the panel driver, rasteriser, font and
procedural backdrop are all written from scratch in this repo.
