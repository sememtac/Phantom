#pragma once
#include <stdint.h>

// ===========================================================================
// THE COCKPIT'S STATE
//
// How the cockpit MOVES, how it ARRIVES, how it takes a HIT, how it goes red at the
// wall, and where a band's work balances across the two cores. None of it is about how
// the drawing is stored; all of it is about the match, and there is one clock running
// it. The renderer -- vg_canopy_op.cpp, the opaque bake -- asks these questions per row
// and per region and keeps its own inner loop.
//
// This was the light-delta renderer, lifted whole out of vg_band.cpp during the systems
// pass. The delta was retired on 2026-09-06 once every hull flew the opaque bake; what
// crossed the boundary between the two renderers while both existed is what is left,
// and it turned out to be the right boundary. See the note at the top of the .cpp.
// ===========================================================================

// The activation regions a drawing may have.
//
// SIXTEEN, which is what the FORMAT can carry: the zone tag is four bits in the bake's
// region runs, so 16 is the ceiling until that changes. The artist decides how many a
// drawing has -- paint the regions, the baker finds them.
#define VG_CANOPY_MAX_ZONES 16

// ---------------------------------------------------------------------------
// What the raster asks of it, per band
// ---------------------------------------------------------------------------

// WHERE THIS BAND'S WORK BALANCES, for the two-core split.
//
// The balance moves with the bend and is the midpoint during the intro, and which
// applies depends on canopy state that used to be read directly out of draw_band. Asked
// for now, so the warp maps and the intro flag stay inside this module.
//
// A band costs the SLOWER half, so an even-looking split of uneven work returns almost
// nothing: measured, the midpoint gave 1.2 of the 1.9 ms it should have.
int vg_canopy_split_at(int band_index);

// Told what the last frame's canopy split actually cost, so the next one can move it. See
// the note at the definition: the two cores are not equally fast, and only a measurement
// knows by how much.
void vg_canopy_split_nudge(uint32_t half_us, uint32_t wait_us);

// ---------------------------------------------------------------------------
// TAKING A HIT, ONE PANEL AT A TIME
//
// The drawing already carries a per-pixel map of which activation region every pixel of the
// SCREEN belongs to -- the artist paints the regions in the green channel and the baker
// turns them into run lists. The intro uses it to bring the cockpit up a region at a time.
// A hit uses the same map to take one back.
//
// HOW MANY REGIONS THERE ARE IS THE ARTIST'S. The baker finds them; sixteen is the format's
// ceiling, not a design choice. More regions means a hit claims a smaller share of the
// view, which is the whole difference between a panel cracking and a quarter of the
// canopy going out.
// ---------------------------------------------------------------------------

// A round struck the ship: pick a panel and mark it. Chooses one that is NOT already
// marked, so successive hits spread across the canopy and stack up rather than refreshing
// one spot -- which is what makes a run of bad luck progressively blind the player.
//
// Does nothing if every panel is already taken; the view is busy enough by then.
void vg_canopy_hit(void);

// Advance every panel's hit, and the static's own clock. Safe every frame of every state.
void vg_canopy_hit_step(float dt);

// Nothing struck, nothing flashing, nothing faulty. For vg_game_init.
void vg_canopy_hit_clear(void);

// ---------------------------------------------------------------------------
// A PANEL THAT NEVER COMES BACK
//
// A hit is an event and heals; damage is a CONDITION and does not. As the hull goes, panels
// start failing for good -- flickering, dropping into static, coming back wrong. The player
// is flying a cockpit that is falling apart around them rather than one that occasionally
// gets hit.
//
// NEVER THE MIDDLE ONE. The central panel is the one being looked through, and taking it out
// is not atmosphere, it is a blindfold. Which panel is central is worked out from the zone
// map when a drawing is selected -- the artist does not have to mark it.
//
// Driven from the hull fraction rather than from damage events, so a ship that limps into a
// match already broken looks it.
void vg_canopy_damage(float hull_frac);

// ---------------------------------------------------------------------------
// Selecting, colouring and bending the drawing
// ---------------------------------------------------------------------------

// The baked cockpit frame, from the author's drawing via tools/canopy_opaque.py. A
// primitive rather than a pass, because WHERE it lands in the order is the point: over
// the world, under the instruments.
void vg_canopy_prim(void);

// THE FRAME FLEXING WITH THE THROTTLE. `k` is 0..1; 0 is rigid.
//
// Cheap because the table is runs, and a run moves by moving its endpoints -- two table reads
// and an add per block, nothing per pixel. Set it once per frame before the flush, from the
// same throttle the instruments' warp uses, so the frame and the panel mounted on it move
// together. See CANOPY_WARP_* in cfg_hud.h and the note at its definition.
void vg_canopy_warp(float k);
// Builds the warp maps vg_canopy_warp asked for. Call once a frame AFTER the band pass
// has drawn, so the rebuild overlaps the transfer rather than delaying it.
void vg_canopy_warp_build(void);
// The amount held by hand, for measurement only. `k` in 0..1 overrides what
// vg_canopy_warp is given on every frame; negative gives the throttle back.
void vg_canopy_warp_pin(float k);
// Told when a different drawing is selected, so the split balance and the centre
// region are worked out again for it.
void vg_canopy_op_changed(void);
// ...and the lag held off. With the bend pinned at zero this is a rigid frame.
void vg_canopy_lag_pin_off(bool off);

// THE FRAME TRAILING THE SHIP. `turn` is the cosmetic bank, and the offset comes from how fast
// it is CHANGING -- so the frame swings during the onset and the release of a turn and sits
// centred through a steady one. Call once a frame, before the flush. Costs one index add per
// panel row. See CANOPY_LAG_* in cfg_hud.h.
// `scale` is the airframe's own shake character -- ShipSpec::shake, which is already 1.70 on a
// CHARIOT and 0.55 on a BALLISTA. The frame of a light hull should be visibly looser than the
// frame of a heavy one, and that ordering is already tuned, so this rides it rather than
// keeping a second table that could disagree with it.
void vg_canopy_lag(float yaw, float pitch, float roll, float scale);

// ---------------------------------------------------------------------------
// THE MOTION
//
// A canopy moves in three ways that have nothing to do with how it is STORED: the
// frame lags the turn on a spring, the tube bends under the throttle, and the roll
// shears it. All three are done by SAMPLING -- the drawing never moves, the
// renderer reads a different part of it -- so any drawing at all can be moved by
// the same numbers, as long as it asks the same questions.
//
// The opaque bake is spans in panel rows, and A PANEL ROW IS A COLUMN, so the two
// questions it needs are which row to read and where along it the reading lands.
// Answered HERE rather than in the renderer because, while there were two renderers,
// two cockpits that computed their own motion were two cockpits that would eventually
// disagree -- and the state this is built from (the spring, the warp maps) is here.
struct VgCanMotion {
    int   row;      // the panel row of the drawing this panel row shows
    int   xofs;     // translation along it: the bow, the pitch lag and the roll shear
    float zbase;    // the sphere's own term for this column
    float zk;       // ...and its dy coefficient, constant for the frame
};

// False when nothing is moving, which is the whole answer: read row for row and
// draw every span where it was baked.
bool vg_canopy_motion(int py, struct VgCanMotion* m);

// ---------------------------------------------------------------------------
// THE ARRIVAL AND THE DAMAGE
//
// Same argument as VgCanMotion above. A cockpit coming online, a panel taking a round
// and a panel that has failed for good are all properties of the MATCH, not of how the
// drawing is stored -- and there is one clock running all three.
//
// `gate_on`     is anything happening at all: the sequence is running, or some panel is
//               hit or faulty. False means skip the whole walk.
// `gate_run`    paints one run of one region with whatever that region is doing. The
//               caller supplies the run; this supplies the state. Rigid on purpose --
//               the gate lands where the VIEW is, not where the frame has swung to.
// `zone_live`   whether a region's cockpit exists yet. True whenever nothing is arriving.
// `zone_glow`   0..255, how hot that region's members are running during the arrival.
//               The renderer spends it on a per-region palette -- see s_hot in
//               vg_canopy_op.cpp -- and on the lit edge.
// THE ORDERED DITHER the reveal is built on, so a second cockpit crosses between two
// states in the same language rather than inventing a fade. One row of the 4x4.
const uint8_t* vg_canopy_bayer_row(int y);

// WHAT THE GATE HAS JUST LEFT IN THE BAND, which is a question about the CLOCK and
// not about any drawing: how far a region has dissolved, 0 held and 255 fully in, and
// what colour a still-held pixel of it is. A caller that knows both can work out the
// value of a held pixel WITHOUT READING IT -- see the note at the arrival's inner loop
// in vg_canopy_op.cpp, and the measured claim above SPAN_BODY that a blended pixel
// costs its read-modify-write and not its arithmetic.
uint8_t  vg_canopy_zone_reveal(int z);
uint16_t vg_canopy_zone_fill(int z);

bool     vg_canopy_gate_on(void);
void     vg_canopy_gate_run(uint16_t* row, int lx, int py, int y0, int n, int z);
bool     vg_canopy_zone_live(int z);
uint8_t  vg_canopy_zone_glow(int z);

// THE COCKPIT COMING ONLINE, at the top of a match.
//
// The view opens black, and it arrives a REGION at a time: the whole region flashes white, the
// world dissolves out of that white, and the frame's members in it run hot and cool to their
// authored level. The order is the artist's, painted into the alpha channel of the drawing --
// see tools/canopy_opaque.py and CANOPY_INTRO_* in cfg_hud.h.
//
// `reset` when a match is BUILT and `begin` when the player takes the seat. Those are NOT the
// same moment and the distinction is load-bearing: a match is built from enter_intro, at the top
// of the cutscene, seconds before there is a cockpit at all.
//
// `update` once a frame with the frame's dt, before the flush. It returns true while the sequence
// is still running, so a caller can hold whatever it wants held; it runs on its own clock and does
// not need the world stopped -- and it looks better if the world is live behind it.
//
// `cued` is a LATCH, set once the RUNNING sequence reaches CANOPY_INTRO_HUD_AT, and it is what the
// instruments hang off so the boot chain follows the cockpit rather than a clock of its own. It has
// to be a latch and not a progress comparison, because the state cannot be inferred from a timer:
// vg_hud_decay is reached from vg_world_step, which the attract loop, the title and the cutscene
// all drive. Inferred from a progress value that reads 1.0 when nothing is running, the condition
// is satisfied by default -- and the cockpit's power-on sound played over the title screen.
//
// `lit` is how many regions have come on so far. It is reported rather than acted on because
// what wants it is a sound, and the rasteriser has no business talking to the mixer.
//
// `flex` is the multiplier to apply to BOTH vg_canopy_warp's k and vg_canopy_lag's scale. It is 0
// while the sequence runs, because the world gate reads the unwarped column and a flexing frame
// would put the black somewhere other than where the panel ends, then ramps to 1 so the cockpit
// takes up its resting bulge instead of snapping into it.
// LOOKING AFT. Set once a frame, before the flush.
//
// Suppresses the cockpit frame and nothing else. The canopy is the front of the ship, so
// drawing it over a view out of the back makes the picture contradict itself.
//
// The intro's world gate is NOT suppressed. It shares this primitive, and it is what holds
// the world black while the cockpit arrives -- so turning the primitive off would show the
// whole world at once, mid-sequence.
void  vg_canopy_rear(bool on);
// Looking aft, and whether the arrival is running. Both are read by the renderer,
// which suppresses its frame in rear view and goes on drawing the world gate while it
// does -- see vg_canopy_op_rows.
bool  vg_canopy_rear_on(void);
bool  vg_canopy_intro_on(void);

// THE WALL WARNING ON THE COCKPIT. `k` is how red, 0 clear and 1 hard against the
// boundary; `white` strobes the frame for a moment and is driven by seconds to impact
// rather than by distance -- see CANOPY_ALARM_SECS.
//
// Costs nothing per pixel: the members are drawn every frame anyway and their colour comes
// from a table, so this is a different table. What it replaced was a red ring over the
// whole view, and that WAS a per-pixel pass -- measured at the boundary, moving the warning
// here took the backdrop fill from 3060 us to 1824 and the frame rate from 53.1 to 59.7.
void  vg_canopy_alarm(float k, bool white);

// WHAT THE WARNING HAS MADE OF THE FRAME'S COLOUR. Returns the base colour -- amber,
// pulled toward COL_DANGER by the clearance, or white for the length of a strobe -- and
// writes 0..1 through `level` for how far along that ramp it is. A strobe reports 1.
//
// Here rather than in the renderer because the alarm's state is here.
uint16_t vg_canopy_alarm_colour(float* level);

void  vg_canopy_intro_reset(void);
void  vg_canopy_intro_begin(void);
bool  vg_canopy_intro_update(float dt);
bool  vg_canopy_intro_cued(void);
int   vg_canopy_intro_lit(void);
float vg_canopy_intro_flex(void);

// WHAT THE DRAWING COSTS, measured rather than predicted. Serial 'k'.
//
// The canopy only draws inside a match, so the frame counter cannot be read without
// someone at the controls -- and what it reports is one band's slower half plus the
// rendezvous, which no table can be judged against. This runs the whole pass on one
// core, rigid and then at full bend, and reports it flat. Needs a drawing selected.
void vg_canopy_op_bench(uint32_t* rigid_us, uint32_t* full_us);

// ---------------------------------------------------------------------------
// SATURATING 565 ADD AND SUBTRACT, in the panel's own byte order
//
// These were the delta renderer's blend, and they outlive it because the blend bench
// in vg_band.cpp prices itself against them. `static inline` rather than published
// storage, so each unit inlines its own copy and the register work behind them is not
// undone by the move -- see THE COMPILER RAN OUT OF REGISTERS, which was 29%.
//
// Four mask constants, not seven. `m - (m >> w)` turns a guard bit into a solid field, so
// one expression saturates with no compare, no branch and no complement constant. The
// input is not masked to 16 bits: every path masks with 0xF81F or 0x07E0 anyway.
// ---------------------------------------------------------------------------

static inline uint32_t px_add(uint32_t s, uint32_t drb, uint32_t dg) {
    const uint32_t v = (s >> 8) | (s << 8);
    uint32_t rb = (v & 0xF81Fu) + drb;
    uint32_t g  = (v & 0x07E0u) + dg;
    const uint32_t mrb = rb & 0x10020u;         // a set guard means that field overflowed
    const uint32_t mg  = g  & 0x00800u;
    rb |= mrb - (mrb >> 5);                     // ...so fill it to the ceiling
    g  |= mg  - (mg  >> 6);
    const uint32_t o = (rb & 0xF81Fu) | (g & 0x07E0u);
    return ((o >> 8) | (o << 8)) & 0xFFFFu;
}

// Subtraction is the same trick run backwards: set the guards first, and a field that
// borrowed through its guard has underflowed. Here a STILL-SET guard is the good case, so
// the same expression becomes a keep-mask and clears the guard on its way out.
static inline uint32_t px_sub(uint32_t s, uint32_t drb, uint32_t dg) {
    const uint32_t v = (s >> 8) | (s << 8);
    uint32_t rb = ((v & 0xF81Fu) | 0x10020u) - drb;
    uint32_t g  = ((v & 0x07E0u) | 0x00800u) - dg;
    const uint32_t mrb = rb & 0x10020u;          // a guard that survived did not borrow
    const uint32_t mg  = g  & 0x00800u;
    rb &= mrb - (mrb >> 5);                      // ...and one that did not, zeroes it
    g  &= mg  - (mg  >> 6);
    const uint32_t o = rb | g;
    return ((o >> 8) | (o << 8)) & 0xFFFFu;
}
