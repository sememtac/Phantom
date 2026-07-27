#pragma once
#include <stdint.h>

// ===========================================================================
// Palette (RGB565).
//
// The CO5300 takes RGB565 big-endian. Rather than byte-swap ~230k pixels per
// frame on the CPU (which is what Arduino_GFX did, and what cost ~13 ms a
// frame), colours are STORED pre-swapped. The rasteriser only ever copies these
// values around, never interprets them, so a band blit is a straight DMA of
// memory as-is. The one function that does interpret a colour, vg_dim(), swaps
// in and back out -- and it runs per object, not per pixel.
//
// Write entries as normal RGB565 and let VGC do the swap at compile time.
// ===========================================================================

#define VGC(x) ((uint16_t)((((x) >> 8) & 0x00FF) | (((x) << 8) & 0xFF00)))

#define COL_BLACK            0x0000      // symmetric; also what memset gives

// --- interface: AmberConsole ----------------------------------------------
// github.com/DutchDiederik/AmberConsole -- exact design tokens, converted.
// Its governing rule is that hierarchy is BRIGHTNESS, inverse video and blink,
// never hue. So the interface is a single-hue intensity ramp and anything that
// needs to shout goes brighter, inverts, or blinks.

#define VG_PAL_AMBER  0    // amber CRT phosphor (P3)
#define VG_PAL_NEON   1    // monochrome AC plasma panel
#define VG_PALETTE    VG_PAL_AMBER

#if VG_PALETTE == VG_PAL_AMBER
#define INK_MAX      VGC(0xFE8A)   // amber-100  #ffd052
#define INK_BRIGHT   VGC(0xFD63)   // amber-90   #ffae1e
#define INK          VGC(0xCC42)   // amber-70   #cd8817
#define INK_FAINT    VGC(0x8AC2)   // amber-50   #8d5b10
#define INK_TRACE    VGC(0x4961)   // amber-30   #4a2f08
#define INK_ONFILL   VGC(0x1860)   // on-fill    #1a0e00
#define INK_WELL     VGC(0x0820)   // screen     #0d0700
#else
#define INK_MAX      VGC(0xFD4D)   // neon-100   #ffa86d
#define INK_BRIGHT   VGC(0xFB41)   // neon-90    #ff6b08
#define INK          VGC(0xDAC0)   // neon-70    #dd5800
#define INK_FAINT    VGC(0xAA20)   // neon-50    #ab4500
#define INK_TRACE    VGC(0x5920)   // neon-30    #5b2500
#define INK_ONFILL   VGC(0x1860)   // on-fill    #1e0c00
#define INK_WELL     VGC(0x0820)   // screen     #100600
#endif

#define COL_HUD              INK_BRIGHT
#define COL_HUD_DIM          INK_FAINT

// --- alerts ---------------------------------------------------------------
// The hull meter is the one place the interface leaves its single hue: the fill
// slides amber -> red as integrity drops. Losing the ship earns the exception.
#define COL_HEALTH_LOW       VGC(0xF900)
#define COL_WARN             VGC(0xF9A0)
#define COL_DANGER           VGC(0xF800)

// --- world ----------------------------------------------------------------
#define COL_STAR_DIM         VGC(0x39E7)
#define COL_STAR_MID         VGC(0x8410)
#define COL_STAR_BRIGHT      VGC(0xFFFF)
#define COL_AST              VGC(0x5FFF)       // pale cyan
#define COL_DEBRIS           VGC(0xFD20)       // orange
#define COL_EXHAUST          VGC(0xFF60)       // amber
#define COL_MOTE             VGC(0xBDF7)

#define COL_ENEMY            VGC(0xFC10)       // hot red
#define COL_ENEMY_HIT        VGC(0xFFFF)

#define COL_MSL_FRIEND       VGC(0x7FFF)       // pale cyan-white
#define COL_MSL_HOSTILE      VGC(0xFC00)       // red-orange
#define COL_TRAIL_FRIEND     VGC(0x5EFF)
#define COL_TRAIL_HOSTILE    VGC(0xFC80)
#define COL_TRAIL_DEAD       VGC(0x4208)       // lock broken: coasting ballistic

// Boundary structure. This is the base BEFORE the distance fade, i.e. how the
// wall looks with your nose against it -- so it is deliberately bright.
#define COL_ARENA            VGC(0x9498)       // light blue-grey

// --- radar ----------------------------------------------------------------
#define COL_RADAR            INK_TRACE
#define COL_RADAR_RIM        INK_FAINT
#define COL_RADAR_BOGEY      VGC(0xF800)       // enemy fighter: red
#define COL_RADAR_MSL        VGC(0xFDA0)       // incoming missile: yellow-orange
