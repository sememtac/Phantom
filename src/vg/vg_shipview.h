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

// The name across the top, and the first field under it.
#define SHIPVIEW_NAME_DY      6
#define SHIPVIEW_HEAD_DY     34

// The clear space between one field and the next. The fields are a BLOCK under
// a caption -- at a wider gap they read as separate lines that happen to be
// stacked.
#define SHIPVIEW_FIELD_GAP    8

// EXTRA SPACE BETWEEN THE LETTERS OF THE NAME. The name is the one string here
// read as a SHAPE rather than as a sentence, and the console's curve closes the
// gaps on one side of the arc enough to weld letters together. 2 at scale 3,
// which is less than the wheel needs at scale 2 because a bigger glyph carries a
// bigger bearing of its own.
#define SHIPVIEW_TITLE_TRACK  2

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
#define SHIPVIEW_CHART_DY   142
#define SHIPVIEW_CHART_R     42

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
#define SHIPVIEW_MODEL_DY   205
#define SHIPVIEW_MODEL_H     58
