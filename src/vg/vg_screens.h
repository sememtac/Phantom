#pragma once
#include "generated/bezel_console.h"   // the chassis, and the windows it leaves
#include "vg_config.h"
#include "vg_input.h"

// ===========================================================================
// Menu screens: ship select, the tournament map, and pause.
//
// Layout lives in this header rather than inside each .cpp because two separate
// things have to agree on it exactly -- the drawing, and the hit tests the state
// machine runs against a tap. Keeping the rectangles in one place is what stops
// a button drifting away from the thing that lights up.
// ===========================================================================

#define MENU_TAP_SLOP   16.0f   // px of travel a contact may drift and still tap

// PX OF DRAG PER DETENT, and it is ONE constant on purpose. The callsign letters
// and the ship wheel are the same mechanism to a thumb, and they would stop being
// the same the first time somebody retuned one of two copies.
#define WHEEL_STEP      26.0f

// --- ship select: a wheel on the left, a detail panel on the right ----------
//
// The left column is the SAME WHEEL the callsign screen uses (vg_entry.cpp), and
// deliberately so: the gesture is learned once, the wheel has no fixed capacity
// so a fifth ship is a table row rather than a layout, and a vertical drag on the
// LEFT is the throttle gesture in flight -- the menu teaches the control.
//
// THE SCREEN IS INSIDE A MACHINE NOW, and the machine decides where it ends.
// BEZEL_CONSOLE_APERTURE_* is the largest rectangle that fits inside the chassis
// art's screen hole, emitted by tools/bezel_bake.py from the drawing itself. The
// layout is derived from it rather than measured against it, so a redrawn
// chassis moves the menu instead of quietly cropping it.
//
// It cost 34px of height. The panel ran to y 396 and the aperture ends at 365,
// so the plan view used to be drawn onto the bottom bezel.
#define SEL_AP_X0       (BEZEL_CONSOLE_APERTURE_X0 + 7)
#define SEL_AP_Y0       (BEZEL_CONSOLE_APERTURE_Y0 + 1)
#define SEL_AP_X1       (BEZEL_CONSOLE_APERTURE_X1 - 7)
#define SEL_AP_Y1       (BEZEL_CONSOLE_APERTURE_Y1 - 1)

// THE SCREEN STAYS DIVIDED: the left strip is the ship wheel and nothing else
// draws over it. What changed is the SIZE of the reading matter in the right
// column, and it changed because of the device.
//
// The panel is 2.16in on the diagonal at 480x480, which is 314 ppi. A scale 1
// glyph is 7px, and 7px there is 0.57mm -- a point and a half. That is not small
// type, it is type nobody can read, and it never showed on the desktop because
// the window is scaled up three times.
//
// Scale 2 is 1.13mm and legible. The right column is 243px of usable width, which
// is TWENTY characters at that scale against weapon descriptions of 25 to 31 --
// so the descriptions WRAP. Wrapping is what keeps the division: the alternative
// was to run the header across the whole glass, and the whole glass includes the
// wheel.
#define SEL_WHEEL_X     SEL_AP_X0
// WIDER, AND THE PANEL PAID FOR IT. The names are tracked now and a tracked
// BALLISTA is 117px against the 96 it used to be, so the window had to grow with
// them or the gain would have been spent on the margins.
//
// 148 and a panel starting at 188 is the most the roster allows, and the limit is
// not the wheel: BALLISTA's WPN row measures 243px and the panel's inside is 244.
// One more pixel of wheel and that line closes its gap; six more and it clips.
#define SEL_WHEEL_W     148
#define SEL_PANEL_X     188
#define SEL_PANEL_W     (SEL_AP_X1 - SEL_PANEL_X)
#define SEL_PANEL_Y     SEL_AP_Y0
#define SEL_PANEL_H     (SEL_AP_Y1 - SEL_AP_Y0)
// A WINDOW, not a column. 168px is the callsign wheel's own height
// (ENT_WHEEL_H) and it is the same number on purpose: two wheels that behave
// identically should look identically sized. A full-height frame around three
// visible rows reads as a list with gaps in it.
#define SEL_WHEEL_H     196
#define SEL_WHEEL_Y     (SEL_PANEL_Y + (SEL_PANEL_H - SEL_WHEEL_H) / 2)
#define SEL_WHEEL_MID   (SEL_WHEEL_Y + SEL_WHEEL_H / 2)   // the detent row
#define SEL_WHEEL_PITCH 44      // px between neighbours on the wheel

// EXTRA SPACE BETWEEN THE LETTERS OF A SHIP NAME.
//
// A name is the one string on this screen that is read as a SHAPE rather than as
// a sentence -- you are picking it out of a list of four, not reading it -- and
// the curve was closing the gaps on one side of the arc enough to weld letters
// together. 3px at the wheel's scale 2, 2 at the panel's scale 3, which is less
// because a bigger glyph already carries a bigger bearing.
#define SEL_NAME_TRACK  3
#define SEL_TITLE_TRACK 2

// HOW MANY NAMES THE WHEEL SHOWS AT ONCE, selection included.
//
// Every class, while the roster is small enough -- a chooser that hides two of
// your four options is asking you to remember them. Capped so that a longer
// roster still shows a WINDOW rather than a list: past five the wheel goes back
// to being a wheel, with more beyond the edges.
#define SEL_WHEEL_SHOWN 5

// WHERE THE DETENT ACTUALLY SITS, and it is NOT the middle of the window.
//
// The block of names is centred, not the selected row: with an even roster the
// rows run one above and two below, and pinning the detent to the middle left the
// block visibly low. An odd roster puts it back in the middle by itself.
//
// Shared, because the DRAW and the HIT TEST have to agree about it. They did not
// for one build, and a tap landing 19px from where the eye says it should is the
// kind of fault nobody reports as a coordinate bug -- it just feels unreliable.
#define SEL_WHEEL_N   ((SHIP_CLASSES < SEL_WHEEL_SHOWN) ? SHIP_CLASSES : SEL_WHEEL_SHOWN)
#define SEL_WHEEL_LO  (-((SEL_WHEEL_N - 1) / 2))
#define SEL_WHEEL_DETENT (SEL_WHEEL_MID - ((2 * SEL_WHEEL_LO + SEL_WHEEL_N - 1) * SEL_WHEEL_PITCH) / 2)
#define SEL_SPINE_W     6       // the selected row's inverse-video spine

// The detail panel, and the chart inside it. The chart centre shares the wheel's
// detent row on purpose: the thing you are choosing and the thing it says line up
// across the screen.
#define SEL_CHART_CX    (SEL_PANEL_X + SEL_PANEL_W / 2)
// Sat on the wheel's detent row until the plan view needed the height. The
// alignment was a nicety; a ship you can actually see is not.
// Pushed down twice. The panel header is FOUR lines now -- name, weapon system,
// tagline, and what the system does -- and SPEED sits directly beneath them. At
// 228 it was 3px clear of the text and at 236 it was 4; neither is a gap.
//
// Both came down when the aperture took the panel's last 34px. The chart gives up
// 6px of radius and the plan view 12px of height, which is the split that keeps
// SPEED clear of the description lines -- the constraint that moved this twice
// already.
// Down 4 to pay for the header, which is spread out: the four lines were 8 / 32 /
// 46 / 58 from the panel top and are 8 / 34 / 50 / 64 now. SPEED has been the
// binding constraint on this number three times, and it still is.
// AND THE CHART GOT ITS RADIUS BACK. Splitting the weapon row in two took both
// fields to one line -- six characters and twelve, against a column that holds
// twenty at scale 2 -- so the header stops at 182 where the wrapped version
// reached 217. R is 42 against the 32 the wrapped version forced, and the plan
// view has 50px against 40.
//
// SPEED sets the ceiling here and has done every time this block has moved. It
// sits at CY - R - 10, so it is the first thing the header runs into: at the
// first attempt at these numbers it was drawn straight through the WPN row, and
// at the second it cleared it by seven pixels and still read as touching it. The
// gap is ten now, which is the fourth time this constant has been moved for that
// one label.
#define SEL_CHART_CY    253
#define SEL_CHART_R     40
// px from a vertex out to its label. Tightened from 13: SPEED sits directly
// under the panel's description lines and was within 3px of them.
#define SEL_CHART_LABEL 10

// The inner margin a labelled field keeps off the panel border. 8 matches the
// rule above the plan view, so the two agree on where the panel's inside is.
#define SEL_FIELD_PAD   8

// THE PLAN VIEW, laid on its side. A top-down silhouette is what a player
// recognises a fighter by, it needs no projection and no per-class 3-D geometry,
// and it suits a slot that is wide and short -- which a spinning object does not.
// See vg_ship_plan.
// Down 4, because the chart's lower labels had come to rest ON the rule above
// the plan view. Moving the chart is what caused it -- DAMAGE and RANGE sit
// 0.809 of the way out, so they fall faster than the centre does.
#define SEL_MODEL_Y     314
#define SEL_MODEL_H     50

// THE TITLE AND THE ENTER KEY ARE IN THE CHASSIS, not on the screen.
//
// The console has a lit window above the aperture and another below it, and they
// are where a terminal puts its banner and its one key. Neither draws a frame of
// its own any more: the metal already is the frame, and a second box inside it
// read as a button sitting on a button.
// How fast the banner crosses its window, in px a second. Slow enough to read a
// word at a time and not so slow that it looks stuck.
// How hard the display bends, as a multiple of the cockpit's own HUD_WARP_K.
// The curve has to be readable as curvature and stop well short of reading as a
// fault, and the ceiling on that is lower than a ladder of screenshots suggests.
// 2.6 is plainly broken -- the wheel box skews and its names leave their column.
// 1.8 survives a still frame and does not survive being looked at: the tilt on
// the wheel names and the bow on the panel border are the first things the eye
// goes to, which is the wrong thing to notice on a screen you are reading.
//
// 1.0 is the cockpit's own bend, and borrowing it turned out to be the wrong
// argument: a HUD is glanced at over a moving world, and this is a page of text
// somebody reads. The same curve that is invisible in flight is legible here as
// wobble, and the words pay for it.
//
// 0.5 leaves the borders visibly bowed -- a straight line is long enough to show
// a gentle curve -- while the arc across one word is under a pixel.
#define SEL_GLASS_WARP  0.5f

// The chord length the curve is cut into on this screen. The cockpit's 64 leaves
// the 266px panel border as five straight pieces with visible joints; 18 reads as
// a curve, at one primitive per chord.
#define SEL_GLASS_SEG   24.0f

// THE FIDUCIAL GRID. Spacing and arm length of the alignment crosses tiled
// across the glass. 74 gives five across and three down inside the aperture,
// which is enough to read as a pattern and few enough that they stay chrome --
// at half this they start to look like content.
#define SEL_TICK_STEP   74
#define SEL_TICK_ARM    3

// How fast the sweep crosses the glass, in px a second. Slow: it is a sign of
// life, not an animation, and anything quick enough to follow with the eye is
// something the eye then has to keep following.
#define SEL_SWEEP_RATE  38.0f

// How far past the aperture the sweep is drawn at each end. The warp pulls a
// point inward in proportion to its distance from the centre, and the ends of a
// full-width line are the furthest points on it -- drawn edge to edge the sweep
// stops short of both edges and reads as cut off. The chassis trims the excess.
#define SEL_SWEEP_OVER  26

// How often the glass is offered a fault, in buckets a second. One in six of
// them takes it, so a tear lands every few seconds. Rare is the entire setting:
// a panel that glitches constantly is a style, one that is clean and then is not
// is broken.
#define SEL_FAULT_RATE  1.4f

#define SEL_CHYRON_RATE 46.0f

// The clear space between one pass of the banner and the next. Three characters
// at the title's own scale: enough that the two readings do not run together,
// short enough that the glass is never empty.
#define SEL_CHYRON_GAP  54

#define SEL_TITLE_CY    ((BEZEL_CONSOLE_BAR_TOP_Y0 + BEZEL_CONSOLE_BAR_TOP_Y1) / 2)
#define SEL_GO_X        BEZEL_CONSOLE_BAR_BOT_X0
#define SEL_GO_Y        BEZEL_CONSOLE_BAR_BOT_Y0
#define SEL_GO_W        (BEZEL_CONSOLE_BAR_BOT_X1 - BEZEL_CONSOLE_BAR_BOT_X0)
#define SEL_GO_H        (BEZEL_CONSOLE_BAR_BOT_Y1 - BEZEL_CONSOLE_BAR_BOT_Y0)

// --- pause -----------------------------------------------------------------
// A STACK, not fixed slots. The number of entries depends on where the pause
// came from -- SKIP exists only in a mode that can be skipped -- and hard-coded
// Y positions would need a new pair of constants for every combination. The
// stack is centred, so adding an entry later moves everything and breaks nothing.
#define PAU_BTN_X       120
#define PAU_BTN_W       240
#define PAU_BTN_H       50
#define PAU_BTN_GAP     12
#define PAU_STACK_CY    300      // the stack's centre, whatever its height

// The config page. It was the audio page, and it grew a display toggle, which is
// why the button that opens it no longer says AUDIO.
#define PAU_SLD_X       100
#define PAU_SLD_W       280
#define PAU_SLD_H       22
#define PAU_SLD_MUSIC_Y 210
#define PAU_SLD_SFX_Y   290
#define PAU_CHK_Y       348      // the scanline box
#define PAU_CHK_SIZE    26
#define PAU_BACK_Y      404

// What the pause screen is offering. Built as a list rather than tested one at a
// time, so the drawing and the hit test cannot disagree about which entry is
// where -- they walk the same list.
enum PauseItem : unsigned char {
    PAUSE_NONE = 0,
    PAUSE_RESUME,
    PAUSE_CONFIG,
    PAUSE_SKIP,      // only in a mode that can be skipped
    PAUSE_QUIT
};

// Fills `out` (4 entries is enough) and returns how many there are.
int  vg_pause_items(bool skippable, PauseItem* out);
// The rect of entry `i` of `n`.
void vg_pause_rect(int i, int n, int* x, int* y, int* w, int* h);
// Which entry is under the point, or PAUSE_NONE.
PauseItem vg_pause_item_at(float x, float y, bool skippable);
// Audio page: which slider is under the point, and the value 0..1 for an x.
bool  vg_pause_music_at(float x, float y);
bool  vg_pause_sfx_at(float x, float y);
bool  vg_pause_back_at(float x, float y);
bool  vg_pause_scanline_at(float x, float y);
float vg_pause_slider_value(float x);

// --- callsign and trail colour ---------------------------------------------
// REGISTRATION IS IN THE SAME MACHINE AS SELECT, so it lives inside the same
// aperture and every one of these came in to meet it. The screen used to have the
// whole panel: title at 30, wheels to 272, the ramp to 396 and NEXT to 458. The
// glass ends at 365 and the banner and the key belong in the plating's own
// windows, so the working height went from 292px to 266.
//
// What gave way was slack, not content. The letter wheels only ever painted
// about 136px of their 168 -- the window is sized to match the ship wheel, not to
// the letters -- so the column kept its size and the space beneath it paid.
#define ENT_COL_W       90
#define ENT_COL_X0      ((SCR_W - 3 * ENT_COL_W) / 2)
#define ENT_WHEEL_Y     (SEL_AP_Y0 + 1)
#define ENT_WHEEL_H     168
#define ENT_TRAIL_X     100
#define ENT_TRAIL_Y     264
#define ENT_TRAIL_W     280
#define ENT_TRAIL_H     44
#define ENT_HUE_X       40
// The handle stands 9px proud of the ramp at both ends, so the ramp has to stop
// 9px short of the glass or the grip is cut off by the steel.
#define ENT_HUE_Y       320
#define ENT_HUE_W       400
#define ENT_HUE_H       30
// Acquiring the ramp should not demand precision -- the slider is a coarse
// choice and the finger covers most of it.
#define ENT_HUE_PAD     26

// HOW MUCH OF THE COLOUR WHEEL THE SLIDER SPENDS ITS WIDTH ON.
//
// vg_hue_col takes a full turn, so a slider mapped straight onto it ran red - yellow -
// green - cyan - blue - magenta - red and arrived back where it started. The two ends
// were the same colour, which wastes the last third of a 400px bar on a journey home
// nobody asked for and makes the extremes impossible to tell apart.
//
// Two thirds of a turn is red through to pure blue: h=2/3 is exactly (0,0,1), so the bar
// ends on a primary rather than part way through a ramp.
//
// vg.trail_hue stays a REAL HUE in 0..1 and is not rescaled -- the save file, the
// opponents' hues and vg_hue_col all keep one meaning for the number. Only the picker is
// restricted, so a profile saved with a magenta hue still draws magenta; its handle just
// pins to the end of the bar.
#define ENT_HUE_SPAN    0.6667f
// The key is the chassis window, exactly as it is on the select screen: one
// machine, one place the key lives.
#define ENT_GO_X        SEL_GO_X
#define ENT_GO_Y        SEL_GO_Y
#define ENT_GO_W        SEL_GO_W
#define ENT_GO_H        SEL_GO_H

// --- tournament map --------------------------------------------------------
// Three buttons now, so they are narrower. The row still starts at 60 and ends
// at 412: the screen is round, and the bottom corners are cut, which is why the
// original two did not run to the edges either.
#define BRK_REP_X       60
#define BRK_REP_Y       414
#define BRK_REP_W       112
#define BRK_REP_H       48
#define BRK_CRS_X       180
#define BRK_CRS_Y       414
#define BRK_CRS_W       112
#define BRK_CRS_H       48
#define BRK_GO_X        300
#define BRK_GO_Y        414
#define BRK_GO_W        112
#define BRK_GO_H        48

// --- repair ----------------------------------------------------------------
#define REP_BAR_X       40
#define REP_BAR_Y       120
#define REP_BAR_W       400
#define REP_BAR_H       44
#define REP_SLIDE_X     40
#define REP_SLIDE_Y     236
#define REP_SLIDE_W     400
#define REP_SLIDE_H     46
#define REP_BUY_X       56
#define REP_BUY_Y       346
#define REP_BUY_W       170
#define REP_BUY_H       54
#define REP_BACK_X      254
#define REP_BACK_Y      346
#define REP_BACK_W      170
#define REP_BACK_H      54

// ===========================================================================
// WHERE THE FINGER IS, FOR THE WHOLE INTERFACE
//
// A control that does not acknowledge a press feels broken before it feels
// slow: you cannot tell whether the machine took the input or whether you
// missed. Every button in the game had that fault, so the fix belongs in one
// place rather than in each screen.
//
// Recorded once a frame in vg_state_update, which is the single entry every
// state passes through, and read by whatever is drawing. That is why no draw
// function had to grow a VgInput parameter: a button already knows its own
// rectangle, so it can ask this whether the press is inside it.
//
// It is the LIVE contact, not a tap. A tap is an event that has already
// finished; this is the finger still being down, which is the thing a lit key
// is reporting.
// ===========================================================================
void vg_press_set(bool held, float x, float y);
bool vg_press_in(int x, int y, int w, int h);

static inline bool vg_in_rect(float x, float y, int rx, int ry, int rw, int rh) {
    return x >= (float)rx && x < (float)(rx + rw) &&
           y >= (float)ry && y < (float)(ry + rh);
}

// --- hit tests, called by the state machine --------------------------------
// Which wheel row a contact hit, as an offset from the detent: 0 the selection,
// -1 the row above, +1 below. SEL_ROW_NONE when the contact missed the wheel.
#define SEL_ROW_NONE 99
int  vg_select_row_at(float x, float y);
bool vg_select_confirm_at(float x, float y);

bool vg_bracket_ready_at(float x, float y);
bool vg_bracket_course_at(float x, float y);
bool vg_bracket_repair_at(float x, float y);

// Screens carrying interaction state of their own -- a wheel mid-drag, a slider
// being held -- own their input handling instead, because that state has nowhere
// sensible to live in the state machine. Each returns true when it is finished.
bool vg_entry_update(const VgInput* in, bool tap, float tx, float ty);
bool vg_repair_update(const VgInput* in, bool tap, float tx, float ty);
void vg_entry_reset(void);
void vg_repair_reset(void);

// --- tournament map view state ---------------------------------------------
void vg_bracket_pan(float dx, float dy);
void vg_bracket_focus_player(void);            // centre on the next match

// --- drawing ---------------------------------------------------------------

// The one button in the game. Drawn in the same idiom as the instrument panels:
// sunk into a dark well, framed, with corner ticks. A primary action is marked
// by a brighter frame and a bright key line under the label -- NOT by filling
// the whole rectangle, because a solid slab reads as a block rather than a
// control and throws away the frame that makes the rest of the interface look
// built.
void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live);

// THE CONSOLE BRACKETS. Every screen bolted into the registration terminal draws
// between these two: open puts up the chassis banner and opens the curve, close
// puts down the key and paints the steel over everything. See vg_screens.cpp.
//
// `note` is the second line in the banner window, or null for a one-line banner
// at the larger size.
void console_open(const char* title, const char* note);
void console_close(const char* key);

void vg_draw_select(void);
void vg_draw_pause(void);
void vg_draw_bracket(void);
void vg_draw_entry(void);
void vg_draw_repair(void);
