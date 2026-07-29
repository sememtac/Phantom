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
// So capture does not run in real time, and does not try to. While it is armed
// the simulation is stepped at a FIXED dt regardless of how long the frame
// actually took, which means the device runs in slow motion while the recording
// plays back perfectly smooth at whatever rate that dt implies. Wall-clock
// speed becomes irrelevant -- it only decides how long you wait, not how the
// video looks. Every frame is captured whole, nothing is dropped, and there is
// no tearing, none of which is true of pointing a camera at the screen.
//
// The trade is that a match cannot really be PLAYED at three frames a second.
// Cutscenes, the menu, the bracket and the attract loop record perfectly
// hands-off; live combat is best captured in short bursts.
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

// Fixed simulation step while armed. Zero when idle, meaning "use real time".
float vg_capture_dt(void);

// Check for a host command. 'c' arms, 's' stops. Cheap enough to call every
// frame.
void vg_capture_poll(void);

// Called by the rasteriser around each finished frame. Bands arrive in the
// order they are drawn, which is top to bottom in panel space.
void vg_capture_frame_begin(void);
void vg_capture_band(int y, int h, const uint16_t* px);
void vg_capture_frame_end(void);
