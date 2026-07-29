#pragma once
#include <stdint.h>

// ===========================================================================
// FRAME CAPTURE
//
// Streams the finished framebuffer to a host over the USB serial link so the
// game can be recorded properly instead of filmed off the panel.
//
// The link is the whole problem: a frame is 480x480x2 = 460,800 bytes, and CDC
// on this part manages roughly a megabyte a second. Two frames per second, and
// there is no compression scheme that turns that into sixty.
//
// There is no way to have both real time and smooth, so there are two modes and
// the host picks:
//
//   LIVE ('l')   The clock is left alone. The game runs at its own speed and
//                what goes out is exactly what was on the panel at that moment,
//                so the recording is real time -- at the handful of frames a
//                second the link can carry, since every send stalls the loop
//                while it goes. Choppy and true. This is the mode for recording
//                someone playing.
//
//   SMOOTH ('c') The simulation is stepped at a FIXED dt regardless of how long
//                the frame actually took, so the device runs in slow motion
//                while the recording plays back perfectly smooth. Wall-clock
//                speed only decides how long you wait. The trade is that a match
//                cannot really be PLAYED at five frames a second -- cutscenes,
//                menus, the bracket and the attract loop record hands-off, live
//                combat is best taken in short bursts.
//
// Both capture every frame whole, with nothing dropped and no tearing, neither
// of which is true of pointing a camera at the screen.
// ===========================================================================

// Wire format. All little-endian, which is what both ends already are.
//
//   frame:  'P','H','F','R', u32 index, u16 w, u16 h, u8 rot, u8 fmt
//   band:   'P','H','B','D', u16 y, u16 h, u32 bytes, payload
//   end:    'P','H','E','N', u32 index
//
// fmt 1 is byte-pair RLE over 16-bit pixels: u8 run (1..255), u16 pixel. The
// pixels are in PANEL byte order and PANEL orientation -- exactly as they go to
// the display -- because that is what already exists in the band buffer, and
// undoing either on the device would cost frame time to save the host a loop.
#define VG_CAP_FMT_RLE16  1

bool vg_capture_active(void);

// The fixed simulation step, in SMOOTH mode only. Zero when live or idle,
// meaning "use the real clock" -- which is the whole of the difference.
float vg_capture_dt(void);

// Check for a host command. 'c' arms smooth, 'l' arms live, 's' stops. Cheap
// enough to call every frame.
void vg_capture_poll(void);

// Called by the rasteriser around each finished frame. Bands arrive in the
// order they are drawn, which is top to bottom in panel space.
void vg_capture_frame_begin(void);
void vg_capture_band(int y, int h, const uint16_t* px);
void vg_capture_frame_end(void);
