#pragma once
#include "cfg_display.h"

// ===========================================================================
// HUD layout and touch zones.
// ===========================================================================

// --- strokes ---------------------------------------------------------------
// Panel frame thickness. AmberConsole specifies 2px, but a 2px bar carried
// through the spherical warp becomes a thin diagonal quad, and the rasteriser
// can land it on a single pixel row -- or drop it entirely -- where the bend is
// steepest. 3px survives that.
#define HUD_STROKE           3

// --- radar -----------------------------------------------------------------
// A half-ellipse dome across the bottom: a circular plan view seen in steep
// perspective. Forward is the top of the dome, the flat chord is the player's
// own 3-9 line. Fighters and incoming missiles only -- asteroids are terrain.
#define RADAR_CX             240.0f
#define RADAR_CY             452.0f
#define RADAR_RX             152.0f
#define RADAR_RY             78.0f
#define RADAR_RANGE          1300.0f  // world units at the rim

// --- throttle --------------------------------------------------------------
// A vertical strip on the LEFT edge, under the left thumb. Contacts that BEGIN
// inside the zone own the throttle; everything to the right steers and fires.
#define THROTTLE_X0          14
#define THROTTLE_W           58
#define THROTTLE_TOP         100
#define THROTTLE_BOT         (SCR_H - 100)

// The touch zone is deliberately wider than the drawn slider so a thumb does not
// have to be accurate to grab it.
#define THROTTLE_ZONE_X1     118

// To ACQUIRE the throttle a contact must land near the slider vertically too.
// The x-zone alone spans the full screen height, so a stray contact anywhere
// down the left edge -- including the bottom corner, which raw (0,0) maps to --
// used to read as "thumb slammed to idle" and silently zeroed the throttle.
// Generous enough (60 px past each end of the drawn control) to still slam.
#define THROTTLE_ZONE_Y0     40
#define THROTTLE_ZONE_Y1     440

// Once a contact owns the throttle it may drag anywhere in the x-zone, but a
// real thumb cannot teleport: at ~65 fps it moves tens of pixels per frame, not
// hundreds. A candidate further than this from its last position is a different
// (or spurious) contact and must not inherit the control.
#define THROTTLE_MAX_JUMP    160.0f

// --- fire button -----------------------------------------------------------
// Missiles launch from a hardware button, not a screen tap. The left thumb owns
// the throttle and the right finger is steering, which leaves the left INDEX
// finger free on the top-edge buttons -- so firing no longer interrupts either
// control.
//
// BOOT (GPIO 0) only. It shipped accepting both GPIOs because the physical
// left/right arrangement was unknown; GPIO 18 turned out to be the +/- key.
//   0x01 = BOOT   0x02 = +/-   0x03 = either
#define FIRE_BUTTON_MASK     0x01
// The other one. Menus only -- currently cycles the ship class on the title card.
#define ALT_BUTTON_MASK      0x02

// A GESTURE SECTION USED TO BE HERE, and what it said stopped being true.
//
// It read "a contact that lifts quickly without travelling counts as a tap, and fires".
// Firing moved to the BOOT button, and the tap path went with it -- vg_player_fire is
// reached from in->fire_edge and from nowhere else. The section had no constants left
// either: the one tap threshold that survives is MENU_TAP_SLOP, and that belongs to the
// menus and lives in vg_screens.h.
//
// Left as a note rather than deleted, because the claim outlived the code by long enough
// to reach the README, and somebody reading only the headers would have believed it.

// --- caution annunciators --------------------------------------------------
// Both alerts flash the same way, so the cadence is defined once, here, rather
// than twice in cfg_world.h and cfg_combat.h. Those keep the DISTANCES, which
// really are a property of the world and of combat; this is a property of the
// panel.
//
// The rate is the range: slow when the thing first matters, fast when it is about
// to happen, and floored.
//
// The floor is 0.5s, which is 2 Hz, and it has been raised three times to get
// there. Aviation human-factors standards put attention-getting flash rates at
// roughly 3-5 Hz, so 0.35s (2.9 Hz) was inside that band -- but those figures are
// for a small lamp on a panel, and photosensitivity guidance caps LARGE flashing
// areas at about 3 Hz. This alert is a filled block on a screen held at arm's
// length, which is the large-area case, so the lower limit is the one that
// applies. A real cockpit also puts the urgency in an aural alert rather than in
// the flash rate, and this game has no sound yet.
//
// 1 Hz at the far edge matches the convention for a caution; 2 Hz at the near
// edge is a warning without being a strobe.
#define ALERT_FLASH_SLOW     1.00f    // seconds per flash at the far edge, 1 Hz
#define ALERT_FLASH_FAST     0.50f    // ...and closest, 2 Hz. Never faster.
#define ALERT_FLASH_DUTY     0.50f    // fraction of the period the block is lit

// --- rear view -------------------------------------------------------------
// A patch in the top right, which is the one part of the panel with nothing in
// it: the throttle owns the left edge, the radar the bottom, and the comms and
// broadcast strips run across the middle.
//
// It shows the same field of view as the main window, at a quarter of the size.
// That is deliberate rather than convenient -- hold the patch and its picture
// fills the screen, and a patch with its own wider field would appear to zoom in
// at the moment the player is trying to judge a closing shot.
#define REAR_W               145
#define REAR_H               44
// Clear of the missile rack, which hud_panel puts at x=440, y=140. The square
// patch this replaced ran to x=442 and y=164 and sat on the corner of it.
#define REAR_X               (SCR_W - SCR_SAFE - REAR_W)
// Flush with the hull bar ON SCREEN, which is not the same as sharing its
// logical y. The hull bar is an instrument: it goes through the spherical warp,
// and with HUD_WARP_K at -0.22 its top edge lands between y=34 (idle) and y=45
// (full throttle). The mirror cannot warp -- it is a viewport, and bending a
// viewport would bend the picture inside it -- so it sits at the warped
// position for cruise. The residual few pixels of throttle-dependent slide are
// the price of one instrument in the row being a window.
#define REAR_Y               40
#define REAR_CX              (REAR_X + REAR_W * 0.5f)
#define REAR_CY              (REAR_Y + REAR_H * 0.5f)
// The warp scale REAR_X and REAR_Y are already tuned for. The patch now rides
// the panel's flex, but only the flex: it is offset by the bend at the current
// scale MINUS the bend at this reference, so at cruise it sits exactly where it
// has always sat and the position above stays the thing that was tuned. Set to
// mid-range so the slide is symmetric about it instead of pulling one way.
#define REAR_WARP_REF        ((HUD_WARP_SPEED_MIN + 1.0f) * 0.5f)
// Same ANGULAR scale as the main window, set by the width. So the picture does
// not change size when the patch is held and fills the screen -- it is the same
// view, uncropped. Vertically that makes the patch a letterbox on it, roughly 20
// degrees against the main window's 61, which is what a mirror is: a wide, short
// band of what is behind you.
#define REAR_FOCAL_K         ((float)REAR_W / (float)SCR_W)

// Touch zone, generous around the drawn patch: it is a button held under a
// thumb, not a control that needs accuracy. A contact here does NOT steer --
// see the partition in vg_input.cpp. Kept off the missile rack.
#define REAR_ZONE_X0         (REAR_X - 24)
#define REAR_ZONE_Y0         (REAR_Y - 30)
#define REAR_ZONE_X1         (REAR_X + REAR_W + 24)
#define REAR_ZONE_Y1         (REAR_Y + REAR_H + 30)

// --- the canopy's own flex ------------------------------------------------
//
// The baked frame is panel-space pixels, so it cannot ride the spherical warp the
// instruments do -- and for a while that was written up as a virtue, the frame staying rigid
// while the panel mounted on it moved. It reads better flexing.
//
// A run can be moved by moving its endpoints, so this costs two table reads and an add per
// BLOCK and nothing at all per pixel. See the note above vg_canopy_warp.
//
// These describe the warp AT REST, because the canopy is inverted against the instruments:
// full bulge with the throttle closed, flattening to nothing as the ship accelerates. See the
// note at the vg_canopy_warp call in vg_render.cpp.
//
// ZOOM is how much BIGGER the frame is at rest, as a fraction, in both axes -- the frame
// sitting close rather than merely stretched. One axis alone made it taller, not nearer.
// SPHERE multiplies HUD_WARP_K so the frame lies on the instruments' own surface. BOW is how
// many pixels the outermost columns shift relative to the middle, on top of the sphere. STEPS
// quantises the amount so the maps are rebuilt a handful of times across a throttle sweep; the
// offsets are whole pixels regardless, so nothing shimmers between steps.
//
// The cost rises with ZOOM, because magnifying reads some columns twice. Being inverted, that
// cost now falls at IDLE, where there is room for it, and full throttle pays nothing.
// SPHERE multiplies HUD_WARP_K, so at 1.0 the frame sits on exactly the surface the
// instruments are drawn on and the two agree. Raise it to bulge the frame more than the panel
// mounted on it; drop it to 0 for a flat zoom.
// SPHERE HAS A CEILING, and it is about 2.27.
//
// The y map's slope is 1 + K*SPHERE*a*(dx^2 + 3dy^2)/R^2. R^2 is CX^2+CY^2 and |dy| is at most
// CY, so the bracket reaches 2.0 -- meaning |K*SPHERE| must stay under 0.5 or the map stops
// being monotone, a run's end lands before its start, and the frame grows holes where it
// folds. HUD_WARP_K is -0.22, so anything past ~2.27 folds. The renderer drops a block whose
// length comes out negative, so the failure is holes rather than corruption, but it is still a
// failure.
//
// ZOOM is the expensive half and SPHERE is nearly free: magnifying makes some columns get read
// twice, while the bulge only moves runs about. Measured, 4086 us rigid, 4922 with zoom 0.13
// and no sphere, 4664 with both -- the sphere REDUCED it, because K is negative and pulls the
// frame inward at the edges. So bulge is the cheap way to get the effect and magnification is
// the dear one.
// TRIED AND REVERTED: sphere 2.0 with zoom 0.06. It measured 316 us CHEAPER than rigid and
// looked worse, which is the whole argument for looking. r^2 concentrates the displacement at
// the EDGES and leaves the middle alone, so a frame dominated by it pinches at the corners
// while the centre of the drawing barely moves. ZOOM shifts everything by the same
// proportion. What reads as the whole canopy warping is mostly ZOOM, with the sphere adding
// curvature on top -- so the balance below is the look, and the cost has to come from
// somewhere else.
#define CANOPY_WARP_SPHERE   1.0f
#define CANOPY_WARP_ZOOM     0.13f
#define CANOPY_WARP_BOW      11.0f
#define CANOPY_WARP_STEPS    12

// --- the canopy trailing the ship -----------------------------------------
//
// PX is how far the frame swings, in pixels, per unit of turn CHANGE. EASE is how quickly the
// smoothed copy catches up: smaller lags longer and swings further. MAX clamps it, because a
// hard reversal can produce a large difference for a frame or two and the frame sliding halfway
// across the screen is not an interior, it is a fault.
// Both are multiplied by the airframe's ShipSpec::shake, so these are the AEGIS figures and
// every other hull is relative to them: CHARIOT 1.70, LANCE 1.30, BALLISTA 0.55. A light frame
// should be visibly looser than a heavy one, and that ordering was already tuned for the
// camera, so the canopy borrows it instead of keeping a second table that could drift out of
// agreement with it.
#define CANOPY_LAG_PX     48.0f
// The spring the frame hangs on. See the note above vg_canopy_lag.
//
// DRIVE is how hard a change in the stick kicks it -- the amplitude dial. SPRING is how long
// the return takes, and it is the one that decides whether the ship feels heavy: 0.030 is a
// period near 36 frames, about six tenths of a second. DAMP decides the overshoot; with this
// SPRING it is a damping ratio around 0.64, so the frame comes home with one soft rebound
// instead of ringing or arriving dead.
//
// Nothing here can move the frame in a single frame, which is the whole point -- the previous
// first-order form snapped back the instant a finger left the glass.
// 0.55 was too loose, on a CHARIOT most of all: what the frame did at a quarter throttle was
// what it should be doing at full. The throttle term is 0.35 + 0.65 * sn, so a quarter is 0.51
// of full -- and that is the factor taken out here, which leaves the RAMP alone and moves the
// whole range down together.
// 0.16, not 0.28: the scale now peaks at agility 1.75 where it used to peak at 1.0, so the
// amplitude the author approved is preserved and only which END of the throttle has it changed.
#define CANOPY_LAG_DRIVE  0.16f
#define CANOPY_LAG_SPRING 0.030f
// Firmer, because "flimsy" is not only amplitude -- a frame that rebounds twice reads as loose
// however far it moves. 0.28 puts the damping ratio near 0.81: home in about the same time,
// with the overshoot mostly gone rather than a visible bounce.
#define CANOPY_LAG_DAMP   0.28f
#define CANOPY_LAG_MAX    14.0f

// Roll relative to the other two. A shear of the same pixel amount reads much stronger than a
// translation of it -- the corners move while the middle does not -- so it is pulled back.
#define CANOPY_LAG_ROLL   0.55f

// HOW MUCH OF THE HULL'S CHARACTER TO USE. ShipSpec::shake spans 0.55 to 1.70, which is the
// right ORDER and too wide a spread for this: a CHARIOT at the full 1.70 was too much while
// the others were right. This compresses toward 1 without disturbing the ranking --
// 1 + (shake - 1) * HULL -- so at 0.6 a CHARIOT is 1.42 and a BALLISTA 0.73.
//
// Compressed rather than lowering CANOPY_LAG_PX, because the complaint was about one hull, and
// dropping the baseline would have taken the three that were already right down with it.
#define CANOPY_LAG_HULL   0.60f

// THE EFFECT FOLLOWS AGILITY, NOT THROTTLE, and it took the author to see why.
//
// It grew with the throttle at first, on the reasoning that speed should feel like something.
// Backwards: agility_slow_bonus adds 75% to an AEGIS's turn rate at a closed throttle and
// agility_fast_malus takes 30% off at full, so the ship turns two and a half times faster with
// the throttle SHUT. The frame answers to angular acceleration, so it should move most where the
// ship turns hardest -- which is idle, not full.
//
// vg.agility is that number, computed each frame from the hull's own bonus and malus, so the
// spread is per-hull for free rather than a curve guessed here. AGI compresses it toward 1: at
// 1.0 the coupling is the flight model's own.
//
// It also now runs WITH the warp rather than against it -- both are strongest at idle. The frame
// is close and busy when the ship is nimble, flat and steady when it is committed to a line.
#define CANOPY_LAG_AGI    1.00f

// --- the canopy coming online ---------------------------------------------
//
// The match opens with the view black and the instruments already lit, and the view arrives a
// REGION at a time: the whole region flashes white, the world dissolves out of that white, and
// the frame's members in it start drawing. The ORDER is the artist's, read out of the green
// channel of the drawing, so this file decides only the pacing.
//
// THE FLASH IS THE WHOLE REGION, not the frame's members inside it. The first version had it the
// other way round and it read as a few struts brightening rather than as a piece of the view
// coming on -- the author's word for the green shapes is "mask", and a mask is the area.
//
// LEAD is how long the black holds before the first region lights, and a full second of it is
// deliberate. The author asked for the wait: arriving in the seat with nothing lit is the moment
// the sequence is FOR, and at a third of a second it read as a dropped frame rather than as a
// system that has not come up yet. Long enough to be uncomfortable is the target.
//
// STEP is the gap between one region and the next. The drawing has four, so the sequence runs
// LEAD + 3*STEP before the last one even starts -- which is why STEP is the constant to reach
// for if the whole thing feels slow.
//
// FLASH is how long the region holds SOLID WHITE before it starts giving way. A flash has an
// instant onset by definition, so there is no ramp into it; this is the plateau only.
//
// Shorter than it looks, and it has to be read together with DISSOLVE. The dissolve begins from
// a fully held region, so the white persists well past this -- the PERCEIVED flash is this plus
// the first part of the dissolve, which is why three frames here is plenty.
//
// DISSOLVE is how long the world takes to come through that white, as an ordered dither rather
// than a cross-fade -- see canopy_gate. This is the part that reads as a region resolving, so it
// wants to be several times FLASH.
//
// SETTLE is the flex ramping in at the end. The frame is held rigid through the sequence because
// the gate and the frame have to agree pixel for pixel, and the resting warp is a long way from
// flat -- so without this the cockpit would jump the moment the intro released. Over half a
// second it reads as the frame taking up its load.
//
// HUD_AT is where in the sequence the INSTRUMENTS are cued, as a fraction of the whole. The boot
// is a chain -- dark, then the cockpit, then the instruments, then the player is in the seat and
// the radio may open -- and before this the three ran concurrently and were, in the author's
// words, overlapping too tightly. Cued off the cockpit's own progress rather than off a timer, so
// retuning the pacing above moves the cue with it instead of quietly sliding it out of step.
//
// 0.75 puts it just after the LAST region has flashed, while that region is still dissolving and
// every member on the panel is still hot -- so the instruments arrive into a cockpit that is lit
// and visibly still coming up, rather than one that has finished and is waiting. At 1.0 the panel
// sits done and empty for a moment first, which reads as a hang.
#define CANOPY_INTRO_HUD_AT    0.65f

// AND THE LAST LINK: how long after SFX_READY before the radio may open at all.
//
// Measured from the SOUND, not from the end of the instruments' flicker, and that is the author's
// specification: one second after the panel's power-on cue, comms may begin. It is a single gate
// -- vg_cockpit.ready -- and BOTH channels wait on it, the broadcast and the opponent alike.
//
// That last part is the fix. The opponent's taunt was gated and the broadcast was not: the course
// briefing ran off a fixed 2.2 s countdown from the top of the match, so it landed wherever it
// landed and moving the HUD cue only moved the collision. Ordering that depends on two timers
// agreeing is not ordering, it is coincidence -- so the briefing waits on the gate too and the
// sequence holds however the pacing above is retuned.
// THE WALL WARNING FLASHES WHEN YOU ARE ABOUT TO HIT IT, and holds steady when you are
// merely near it. Two signals, from two different facts, because one number cannot carry
// both.
//
// A steady ramp from amber to red says "closer" and never says "now". The author's note:
// without a change of KIND rather than of degree, there is no way to read which shade of
// red means you are out of room, so the whole thing reads as decoration.
//
//   the COLOUR comes from the clearance   -- how near the boundary is
//   the FLASHING comes from vg_wall.rate  -- how soon you reach it at this rate
//
// So holding station near the wall is red and calm, and diving at it flashes from further
// out. A BALLISTA at full throttle trips it earlier than a CHARIOT easing along, without
// either being a special case: the rate does that on its own.
//
// SECONDS TO IMPACT, not a fraction of a range. It is the honest unit -- the player is being
// told how long they have -- and it makes the threshold mean the same thing on every hull at
// every speed, which a distance never can.
#define CANOPY_ALARM_SECS     1.6f    // start flashing this far from impact
#define CANOPY_ALARM_SECS_MAX 0.4f    // ...and reach the top rate here
#define CANOPY_ALARM_RATE_MIN 8.0f    // units a second of closing before it counts at all

// THE STROBE. White for this long, then back to whatever the clearance asked for.
//
// A DUTY CYCLE AND NOT A WAVE. A sine spends half its time near the top, which reads as the
// frame being generally brighter; a short spike with a long gap reads as a flash. 45 ms is
// about three frames at sixty -- long enough to register, short enough that the red is what
// the eye spends its time on.
#define CANOPY_ALARM_WHITE    0xFFFF  // what a strobe goes to
#define CANOPY_ALARM_ON_SECS  0.045f  // how long each flash lasts
#define CANOPY_ALARM_HZ_MIN   2.5f    // flashes a second at the threshold
#define CANOPY_ALARM_HZ_MAX   9.0f    // ...and hard against the wall

#define BOOT_RADIO_WAIT        1.00f

// The opponent's OPENING line, after the radio has opened. The broadcast speaks first where there
// is one -- a course briefing is information and a taunt is flavour -- so this trails the gate
// rather than sitting on it.
#define BOOT_FIRST_TAUNT       0.80f
// ONE DIAL FOR THE WHOLE SEQUENCE'S SPEED, because it has been retuned three times and four
// separate edits would eventually disagree with each other. Above 1 is faster: 1.25 runs the
// regions through a quarter quicker than the authored numbers below.
//
// It scales the REGIONS -- the gap between them, the flash, the dissolve and the members cooling.
// It deliberately does NOT scale LEAD, because the second of dark at the start is not pacing, it
// is the thing the sequence is for and the author set it by hand. Nor SETTLE, which is the frame
// taking up its flex after everything else is done.
//
// The authored numbers stay visible underneath, so what the sequence was designed at is still
// legible after the rate has been moved. And CANOPY_INTRO_HUD_AT is a FRACTION, so the
// instruments' cue rides this automatically rather than needing its own correction.
#define CANOPY_INTRO_RATE      1.25f

#define CANOPY_INTRO_LEAD      1.00f
#define CANOPY_INTRO_STEP      (0.30f / CANOPY_INTRO_RATE)
#define CANOPY_INTRO_FLASH     (0.03f / CANOPY_INTRO_RATE)
#define CANOPY_INTRO_DISSOLVE  (0.32f / CANOPY_INTRO_RATE)
#define CANOPY_INTRO_SETTLE    0.55f

// THE FLASH COLOUR, as RGB565 in PANEL byte order -- it is stored straight into the band buffer,
// which is what the wire reads. Full white is symmetric, so 0xFFFF needs no swap.
//
// Turn it down here if the flash blows out on the panel. The five bits of red and blue and six of
// green are 0..31 and 0..63, so a restrained white is something like (28, 58, 28) -- and being a
// flat fill rather than an additive blend, whatever is put here is exactly what appears. It
// cannot clip, which is the point: the previous version reached white by lerping the panel's
// orange in an additive table, passed through a hot salmon on the way, and blew out the hue.
#define CANOPY_INTRO_WHITE     0xFFFFu

// THE MEMBERS LIGHT UP, and this one is a bug the author kept.
//
// The first version flashed the frame's members instead of the region and blew the hue out doing
// it. Wrong area -- but the blow-out itself looked good, so it is deliberate now and it runs on
// the DISSOLVE, where there is something behind the members for it to clip against.
//
// LIT is how long a region's members take to cool from white-hot to their authored level, timed
// from the end of its DISSOLVE. They hold full heat until then, and the timing is the whole
// trick: a member is invisible against its own region's white fill, so cooling them from the
// moment the region lit meant the glow peaked with nothing behind it to clip against and was
// already gone by the time world cells came through. The heat has to outlast the dissolve,
// because the world is what it saturates against. The frame is the LAST thing to settle.
//
// PEAK is how crazy it gets, and it is the interesting dial. The colour comes from SATURATION:
// red has five bits and clips first, green has six and holds longer, so a rising white delta over
// a lit world passes through magenta and amber on its way out. At 1.0 the members start fully
// white -- which is the one part of the ramp with no colour in it at all -- so pulling this DOWN
// gives more hue, not less. Below about 0.5 it stops reading as heat.
#define CANOPY_INTRO_LIT       (0.45f / CANOPY_INTRO_RATE)
#define CANOPY_INTRO_LIT_PEAK  0.85f

// Quantised, for the reason everything else here is: the per-zone colour table is rebuilt when a
// glow changes, and a float would rebuild all four every frame. At 24 steps over LIT's third of
// a second a step lasts about a frame, and the ramp is a colour sweep rather than a brightness,
// so nothing reads as stepped.
#define CANOPY_INTRO_QSTEP     24

// HOW BIG THE LOCK RING IS DRAWN, as a fraction of the cone it stands for.
//
// 1.0, and it was 0.60. The ring was drawn inside the cone so that everything
// within it was certainly locked -- which meant the angle the game enforced was
// WIDER than the one on the panel, and a target could be held a little outside the
// circle. Reported from the cockpit, and it is the wrong compromise for this
// class: the circle IS the mechanic, so the circle has to be the boundary. The
// cone was tightened to match the ring rather than the ring loosened to match the
// cone, so the size on the panel did not change.
#define LOCK_RING_K          1.00f

// THE RANGE AT WHICH THE CIRCLE IS THE CLASS'S OWN CONE. Closer than this it stops
// widening; further, it narrows as 1/range.
//
// A FIXED ANGLE GETS EASIER THE FURTHER YOU ARE, which is the fault this repairs.
// The same sideways jink subtends a smaller angle at long range, so a target two
// thousand units away barely moves inside the circle and sniping is free.
// Narrowing as 1/range fixes the escape in WORLD units instead: about 85 of
// lateral movement shakes the lock at any distance at all, so a pilot being shot
// at always has the same physical move available and the shooter always has the
// same physical tolerance to hold. Reported from the cockpit -- up close the
// circle felt right, at range it was a freebie.
#define LOCK_TIGHTEN_REF     600.0f
