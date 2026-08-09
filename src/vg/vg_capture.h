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
// Nothing arms this directly any more. Two modes used to: LIVE, which sent
// whatever was on the panel and stalled the game to 15fps doing it, and SMOOTH,
// which stepped the simulation on a fake clock so the video was 60fps but the
// board was in slow motion. Neither could hold a real 60fps, so neither
// survived -- see vg_replay.h, which records the SIMULATION at full speed and
// reads the pixels back afterwards.
//
// What remains is the frame streaming itself, driven by replay.
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

enum { VG_CAP_OFF = 0, VG_CAP_STREAM = 1 };

bool vg_capture_active(void);

// Force a mode. Used by replay, which drives its own clock and needs every
// rendered frame streamed without the host arming capture separately.
void vg_capture_set(int mode);

// True when the link is carrying binary and MUST NOT be printed to. Any log
// line landing between two bands is indistinguishable from corrupt pixel data,
// and the host has no way to tell them apart -- one stray printf ends the
// recording. Guard every print that can fire during a frame with this.
bool vg_link_busy(void);

// Write every byte, however many calls that takes. Serial.write returns a count
// and short-writes under load; anything binary must go through this or it will
// be truncated in a way neither end can detect.
void vg_link_write(const void* p, int n);

// Let writes block, or not. On for the duration of a session only.
void vg_link_blocking(bool on);

// Send the core's own log output nowhere while a capture or a replay owns the port. Call once
// at startup. See the note at its definition -- an I2S error at boot cost a whole recording.
void vg_link_guard_logs(void);

// Put this frame's audio into the capture stream. Called once per frame with
// whatever the mixer produced, and does nothing unless a capture is running.
void vg_capture_audio(const int16_t* samples, int n);
// Forget that a host asked for audio. Called when a session ends, so the next
// one starts from silence and has to ask again.
void vg_capture_audio_off(void);

// What the write path actually did. `bytes` is what was handed to it; if the
// host received fewer than this, the loss happened after the device, which is
// the only way to tell the two apart.
void vg_link_stats(uint32_t* bytes, uint32_t* shorts, uint32_t* stalls,
                   uint32_t* mismatch);
void vg_link_stats_reset(void);
void vg_capture_frame_counts(uint32_t* begins, uint32_t* ends);

// Check for a host command: 's' stops a stream, 'b' benchmarks the link, and
// the replay commands are dispatched from here. Cheap enough to call every
// frame.
void vg_capture_poll(void);

// ONE FULL BREAKDOWN, ON REQUEST. Serial 'd'.
//
// The deep splits -- the update's eleven spans, group B's parts, the world's six, the grid
// and the I2C -- used to print every two seconds alongside everything else, which came to
// twelve lines a window. Most of them had already answered the question they were added
// for, and a line nobody reads is worse than no line: it pushes the ones that matter off
// the top of the terminal.
//
// So the periodic report is what you WATCH and this is what you ASK. Returns true once, for
// the next window only.
bool vg_capture_want_detail(void);

// Called by the rasteriser around each finished frame. Bands arrive in the
// order they are drawn, which is top to bottom in panel space.
void vg_capture_frame_begin(void);
void vg_capture_band(int y, int h, const uint16_t* px);
void vg_capture_frame_end(void);
