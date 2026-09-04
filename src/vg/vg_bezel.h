#pragma once
#include <stdint.h>

// ===========================================================================
// A BEZEL: THE CONSOLE THE MENU IS BOLTED INTO
//
// The fantasy is a registration terminal -- you are standing at a machine in a
// room, filing your callsign and your hull before a tournament. The bezel is the
// machine: the metal chassis, the cable runs, the warning plates, the two lit
// strips above and below the screen. The menu is what the screen is showing.
//
// IT IS NOT A CANOPY, and the difference is the whole reason this file exists
// rather than a fifth entry in the canopy set. A canopy is a light DELTA applied
// to the finished picture, which is right for a cockpit frame: the frame catches
// whatever the world behind it is doing, and a canopy over a nebula glows with
// the nebula. That is exactly wrong for a machine in a room. Chassis metal is
// opaque. The stars do not shine through it, and painting it additively over the
// idle scene would have shown them doing precisely that.
//
// So a bezel stores COLOUR and replaces the pixel. It costs a byte a pixel
// instead of the canopy's signed delta, and it is cheaper to draw, because
// replacing a pixel is a store where adding to one is a read, an add, a clamp
// and a store.
//
// WHAT IS NOT STORED. The artist paints the area the game fills itself in
// magenta -- the screen aperture and the two bar windows. The baker keeps no
// pixel there. That is 58% of the panel, and it is the reason a full-screen
// opaque backdrop is affordable at all: only the frame around the hole is real.
//
// PANEL ROWS, NOT PICTURE ROWS. The display is mounted a quarter turn from the
// drawing, so the baker rotates once and the firmware reads straight lines.
// Every rotation the renderer would otherwise do per frame was done once, by a
// tool, on a desktop.
//
// Nothing here is owned or copied. A bezel lives in flash and is pointed at.
// ===========================================================================

// WHAT A PAINTED AREA IS FOR. The artist paints the holes in the chassis in the
// colour of the job they do, and the baker carries that through, so a screen asks
// the drawing where its headline goes rather than being told a number.
//
// The roles used to be worked out from geometry -- largest region is the screen,
// sort the rest by position -- which is inference dressed up as a rule, and it
// put the two bar windows the wrong way round on the first drawing.
enum : uint8_t {
    VG_SLOT_DRAW = 0,       // magenta: the game draws here and decides what
    VG_SLOT_HEADLINE = 1,   // cyan:    the ticker runs across here
};

// One hole, and TWO rectangles for it, because they answer different questions.
//
//   INNER  the largest rectangle that fits inside the hole. What a hit test and a
//          layout want -- everything in it is really in the hole.
//   BOX    the hole's full extent, chamfered corners included. What a fill and a
//          clip want: filling only the inner rectangle leaves the corners
//          unpainted, and clipping to it cuts a moving label short of the glass.
struct VgBezelSlot {
    uint8_t role;
    int16_t x0, y0, x1, y1;
    int16_t bx0, by0, bx1, by1;
};

// One run of stored pixels in a panel row, between two exempt areas.
struct VgBezelSpan {
    uint16_t x0;        // where the run starts
    uint16_t len;       // how many pixels it covers
    uint32_t off;       // where its pixels start in data
};

struct VgBezel {
    const uint16_t*    pal;     // RGB565, one entry per palette index
    const uint8_t*     data;    // one palette index per stored pixel
    const VgBezelSpan* span;    // every run, in panel row order
    const uint16_t*    row;     // first span of each row; SCR_H + 1 entries
    const VgBezelSlot* slot;    // the holes, by role then reading order

    uint16_t spans;
    uint32_t pixels;
    uint8_t  slots;
};

// The Nth hole the game draws in, top to bottom then left to right, or null.
// A drawing with three keys along the bottom hands them over left to right, so a
// screen names them by position and never by coordinate.
const VgBezelSlot* vg_bezel_slot(int n);

// The headline hole, or null if the drawing has none.
const VgBezelSlot* vg_bezel_headline(void);

// WHICH BEZEL, or none. A menu that wants the console asks for it and a menu
// that does not gets nothing drawn, so this costs a null test on screens that
// have no chassis.
void          vg_bezel_use(const VgBezel* b);
const VgBezel* vg_bezel_current(void);

// Submit the bezel to the primitive list. Call it BEFORE the screen draws, so
// the menu lands on top of the chassis rather than under it.
void vg_bezel_prim(void);

// The GAPS in a panel row: the stretches the chassis does NOT paint, which are
// the screen and the two windows. Writes up to VG_BEZEL_MAX_GAPS pairs and
// returns how many. The scanline pass uses it to stay on the glass.
#define VG_BEZEL_MAX_GAPS 5
int vg_bezel_gaps(int y, int16_t* out);

// Paint this core's rows of one band. Called from the band raster.
void vg_bezel_rows(uint16_t* band, int by0, int r0, int r1);
