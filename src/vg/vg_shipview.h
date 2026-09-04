#pragma once
#include <stdint.h>
#include "vg_ship.h"

// ===========================================================================
// WHAT A SHIP CLASS LOOKS LIKE, DRAWN.
//
// The name, what the class carries, the five axes as a shape, and the hull seen
// from above. It was the right half of the ship select screen and nothing else
// could ask for any of it -- which was fine while ship select was the only
// screen that ever had to say what a class IS. The tournament table has to say
// it about an opponent, and the launch cutscene already says a third of it in
// words of its own.
//
// SO IT TAKES A RECTANGLE. Everything inside is placed against that box rather
// than against the panel it grew up in: the offsets are the same numbers,
// measured from the top of the box instead of from the top of the screen. The
// same panel drawn somewhere else is then the same panel, and not a second copy
// that drifts away from this one the first time either is touched.
//
// It knows what a ShipSpec is and it does not know what a screen is -- the rule
// vg_ui lives by, one layer up. These widgets are about the roster.
// ===========================================================================

// THE PANEL'S OWN MEMORY, and the CALLER owns it.
//
// Changing class is an animation whose two halves behave differently -- see the
// note above the transition in the .cpp -- so the panel has to remember what it
// was showing. That was four file statics, which is exactly right for one panel
// on screen at a time. A tournament page showing you and your opponent side by
// side is two, and two panels sharing one tween would drag each other back to
// their own class every frame.
struct VgShipView {
    int8_t to;          // the class it is settling on
    int8_t from;        // ...and the one it is leaving
    float  t0;          // when the change came, on vg.state_t
    float  ax[5];       // the chart AS SHOWN when it came
};

// A panel that has shown nothing yet, so the first class it is given arrives
// without a transition rather than sweeping in from nowhere.
void vg_shipview_reset(VgShipView* v);

// ---------------------------------------------------------------------------
// A CLASS AS A MARK, for somewhere far too small to draw the hull.
//
// The bracket sheet has about sixteen pixels a side to say what somebody flies,
// which is a tenth of what the plan view needs to be recognisable. It carried one
// letter of the class name instead -- A, L, C, B -- and at that size a letter is
// something you READ, one box at a time, when what the sheet is for is taking in
// a column of opponents at a glance.
//
// So: symbology. Four marks, each drawn from the single quality the class is
// named for in vg_ship.h, in the register a military display would use --
// outlines rather than solids, because this renderer has no polygon fill and a
// filled slab is against the house style in any case.
//
//   AEGIS     the shield   a dome on its base
//   LANCE     the point    a dart, apex forward
//   CHARIOT   the speed    two chevrons, swept back
//   BALLISTA  the range    a bolt on a rail
//
// `hw` and `hh` are HALF extents from the centre, so the caller sizes the mark to
// the cell it has rather than to a scale nobody can check.
void vg_ship_glyph(ShipClass c, int cx, int cy, int hw, int hh, uint16_t col);

// Draw class `cls` in the rectangle. `v` carries the change from frame to frame.
void vg_shipview_draw(VgShipView* v, int cls, int x, int y, int w, int h);

// ---------------------------------------------------------------------------
// THE FIVE AXES, AS NUMBERS. SPEED / HULL / RANGE / DAMAGE / RATE, each 0..1,
// written into out[5] in that order and read straight off the spec table.
//
// It lived in vg_ship.h, which made the ROSTER depend on cfg_hud.h for the
// display ranges it normalises against -- the data reaching up into a screen's
// tuning to answer a question that only the drawing asks. Nothing outside this
// file ever called it.
//
// The bars it replaced were hand-normalised and had gone false: damage was
// divided by 44 while BALLISTA carries 120 and AEGIS 50, so BOTH clamped to a
// full bar and a 2.4x difference showed as no difference at all. Derived from
// the spec, a retune moves the chart with it.
void vg_ship_axes(const ShipSpec* sp, float out[5]);

// What each axis is called, in the same order.
const char* vg_ship_axis_name(int i);

// ---------------------------------------------------------------------------
// WHERE THINGS SIT INSIDE THE BOX.
//
// OFFSETS FROM THE TOP OF THE PANEL, not screen coordinates. They were absolute
// -- the chart's centre was 241 and the plan view began at 304 -- against a
// panel whose top has been 99 since the chassis art fixed it. These are those
// numbers less 99, so the drawing is unchanged and the box can now be somewhere
// else.
//
// The reasoning behind each is with the code that spends it, and it is worth
// reading before moving one. SPEED has been the binding constraint on the
// chart's centre five separate times.

// The inner margin a labelled field keeps off the panel border. 8 matches the
// rule above the plan view, so the two agree on where the panel's inside is.
#define SHIPVIEW_FIELD_PAD    8

// THE NAME IS NOT ON THIS PANEL ANY MORE.
//
// The wheel two inches to the left carries it at scale 3 in an inverse-video
// detent, which is the largest word on the screen and the thing being chosen. The
// panel repeated it at the same size directly beside it, and a fact stated twice
// on one screen is not emphasis, it is a fact taking up room that something else
// needed.
//
// It cost the header 26 pixels of the 265 this panel has, and the chart and the
// hull have taken them: the radius is 52 where it was 42, and the plan slot is 66
// where it was 58. That is the first time either has been given anything back
// since the aperture took 34px off the bottom.
//
// The first field, from the top of the panel.
#define SHIPVIEW_HEAD_DY      8

// The clear space between one field and the next. The fields are a BLOCK under
// a caption -- at a wider gap they read as separate lines that happen to be
// stacked.
#define SHIPVIEW_FIELD_GAP    8

// THE CHART'S CENTRE, and this offset has moved five times for one label.
//
// SPEED sits at CY - R - 10, so the header above is the first thing it runs
// into: at the first attempt it was drawn straight through the WPN row, and at
// the second it cleared it by seven pixels and still read as touching. The gap
// is sixteen now. Ten measured as clear and still read as joined, which is the
// whole lesson -- the number that matters is not the gap, it is whether the eye
// reads two lines or one block.
//
// The radius came back when the weapon row was split in two: both fields went to
// one line, so the header stops at 182 where the wrapped version reached 217.
// SIX MORE OF RADIUS, paid for by the name leaving -- and the first attempt took
// ten, which was too many.
//
// SPEED sets the ceiling and DAMAGE and RANGE set the floor, and this block has
// now been moved SIX times for one or the other of them. At R 52 the lower pair
// finished three pixels off the rule above the plan view, which is the same
// distance the panel has quietly lived with for months and the same thing the
// note below records being fixed once already: measured as clear, read as
// touching.
//
// So the radius takes six of the reclaimed pixels and the GAP takes the rest.
// The lower labels clear the rule by ten now and SPEED clears the WPN row by
// thirteen. That is the first time either end of this chart has had room rather
// than a tolerance.
#define SHIPVIEW_CHART_DY   123
#define SHIPVIEW_CHART_R     48

// px from a vertex out to its label. Tightened from 13: SPEED sits directly
// under the panel's description lines and was within 3px of them.
#define SHIPVIEW_CHART_LABEL 10

// THE PLAN VIEW, laid on its side. A top-down silhouette is what a player
// recognises a fighter by, it needs no projection and no per-class 3-D geometry,
// and it suits a slot that is wide and short -- which a spinning object does not.
//
// The hull needed the last move up: BALLISTA's reverse wing is the widest thing
// in the roster and the plan view is scaled so that wing fills the slot, so the
// tip was landing two pixels from the panel's bottom border and crossing it. The
// chart gave up three of radius to pay for it.
// EIGHT MORE OF HEIGHT, and the hull is scaled off it, so the silhouette and the
// mark beside it both grow with the slot. The bottom stays two pixels clear of
// the panel border, which is what BALLISTA's reverse wing needs.
#define SHIPVIEW_MODEL_DY   197
#define SHIPVIEW_MODEL_H     66

// The class mark's half extents, beside the hull. Bigger than the bracket's,
// because here it is being TAUGHT rather than read at a glance and there is room
// for it to be looked at.
#define SHIPVIEW_GLYPH_HW    11
#define SHIPVIEW_GLYPH_HH     9

// The clear space between the tail of the hull and the mark.
//
// They have to read as a pair, which is the whole point of drawing them together
// -- but they are two DIFFERENT KINDS of thing, a picture of a ship and a sign
// standing for one, and at nine pixels the mark read as part of the hull's own
// geometry rather than as a label on it. Far enough apart to be two objects,
// close enough to be one pairing.
#define SHIPVIEW_GLYPH_GAP   18
