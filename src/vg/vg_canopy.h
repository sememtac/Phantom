#pragma once
#include <stdint.h>

// ===========================================================================
// A CANOPY, AS A DATA OBJECT
//
// One drawing baked by tools/canopy_bake.py becomes one of these. It used to be a
// set of CANOPY_* macros, which worked for exactly as long as there was one of
// them: a macro cannot be selected at runtime, so a second cockpit could not
// exist. Every hull gets its own now, and the artist's job stops at supplying a
// drawing.
//
// The three tables are described where they are generated, but the division is
// the important part and it is not obvious:
//
//   ofs/data    the FLIGHT table. Runs are NOT cut at zone borders, so a block
//               may span two zones and its zone tag means nothing. This is the
//               one read on every frame of every flight, and it is deliberately
//               cheaper than the intro's.
//   iofs/idata  the INTRO table. Same drawing, every run cut at its zone border,
//               so switching a region on is switching whole blocks on. Costs
//               ~35% more blocks, which is why it is not the one used in flight.
//   zofs/zdata  the ZONE MAP, over every pixel of the SCREEN and not just the
//               frame. The intro holds the world black a region at a time, and
//               the block tables only describe the tenth of the screen the
//               cockpit covers -- they cannot answer for the rest.
//
// Nothing here is owned or copied. A canopy lives in flash and is pointed at.
// ===========================================================================

// The activation regions a drawing may have.
//
// SIXTEEN, which is what the FORMAT can carry: the zone tag is bits 5..2 of a block
// header and of a zone-map run, so 16 is the ceiling until that changes. It was 8, which
// was half of what the data could already express.
//
// The artist decides how many a drawing has -- paint the regions, the baker finds them.
// What each one costs is not uniform: a zone needs four bytes of gate state, which is
// nothing, and a 256-entry colour table ONLY because the intro glows each region
// separately. The table is the 512 bytes; the flash and the dissolve are the four.
#define VG_CANOPY_MAX_ZONES 16

struct VgCanopy {
    const uint16_t* ofs;        // flight table: column -> byte offset, cols+1 entries
    const uint8_t*  data;
    const uint16_t* iofs;       // intro table, same shape
    const uint8_t*  idata;
    const uint16_t* zofs;       // zone map, one run list per screen column
    const uint8_t*  zdata;
    const uint8_t*  split;      // where each band's work balances, NUM_BANDS entries

    uint16_t cols;              // columns stored; half the screen if mirrored
    uint8_t  bg;                // the grey that means "no change"
    uint8_t  mirror;            // the left half is stored and reflected
    uint8_t  zones;             // activation regions found in the green channel

    uint16_t blocks;            // what the pass gets through, for the bench
    uint16_t flat_px, lit_px;
};

// WHICH COCKPIT IS BEING FLOWN. Call on entering a match, before the first flush.
//
// Rebuilding the colour table is this function's job because `bg` is per drawing:
// two cockpits with different background levels turn the same stored grey into
// different amounts of light, so a table left over from the previous hull would
// draw this one at the wrong brightness. Cheap -- 256 entries, once per match.
//
// Passing null is ignored rather than fatal. A hull whose drawing has not been
// authored yet keeps whatever was selected last, which is a wrong cockpit rather
// than no cockpit, and a wrong cockpit is something the artist can see.
void vg_canopy_use(const VgCanopy* c);

// Which one is selected, for the bench and the baker's own checks.
const VgCanopy* vg_canopy_current(void);
