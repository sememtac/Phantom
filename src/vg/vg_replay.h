#pragma once
#include <stdint.h>
#include "vg_input.h"

// ===========================================================================
// SESSION RECORD AND REPLAY
//
// The way to record gameplay at 60fps over a link that can carry 23.
//
// Pixels are hopeless: a frame is 460,800 bytes and the wire does 0.74 MB/s.
// But the SIMULATION is tiny -- a dt and an input struct, under a hundred
// bytes a frame, five kilobytes a second. So recording logs that instead, the
// game runs at its true unimpeded 60fps while you play, and afterwards the
// device re-runs the session frame by frame and streams the pixels at whatever
// speed the link likes. The video comes out at a real 60fps because the frames
// really were 1/60s apart when they happened.
//
// This works only because the simulation is a pure function of (seed, dt,
// input). Two things had to be true and both already were: the game draws from
// a seeded xorshift rather than the hardware RNG, and no game or render code
// reads the wall clock. The four esp_random() calls that DO exist are routed
// through vg_replay_rand() below, which logs them while recording and hands the
// same values back while replaying.
//
// The remaining input is persisted progress -- credits, callsign, ship,
// champion, trail hue. Those are snapshotted into the session header and
// restored on playback, and saving to flash is suppressed while replaying, so
// a replay cannot rewrite the progress of the person who recorded it.
//
// What is NOT captured is anything the player could not have affected: touch
// and IMU are never read during playback, because the recorded VgInput already
// contains everything they produced.
// ===========================================================================

enum VgReplayMode { VG_RP_OFF = 0, VG_RP_RECORD, VG_RP_PLAY };

int  vg_replay_mode(void);

// Stands in for esp_random() at the four sites that seed the world. Logged
// while recording, replayed while playing, passed straight through otherwise.
uint32_t vg_replay_rand(void);

// Host command from the capture poller: 'R' record, 'P' play, 'T' timed play,
// 'E' end. Returns true if the byte was a replay command and has been handled.
bool vg_replay_command(int c);

// A TIMED REPLAY: the same session, frame for frame, WITHOUT sending the pixels.
//
// 'P' streams every replayed frame, which is the whole point of it -- but it also means the
// link paces the run at about 17 frames a second and the host spends five minutes decoding
// 460 KB a frame. None of that is needed to answer "what does this drawing cost".
//
// 'T' replays identically and streams nothing. The counters it accumulates are CPU WORK --
// how long the raster spent on each kind of primitive -- so they are unaffected by the host
// pacing the run. Wall-clock numbers are NOT: with no pixels to send, the panel is never the
// bottleneck, so frame time and `blit` under a timed replay mean nothing and are not reported.
//
// This exists because the same scene twice is the only honest way to compare two drawings.
// Every attempt in this project to compare canopy cost between two live fights has been
// wrong, in both directions, because the scene moved underneath the comparison. A replay
// cannot move: identical seeds, identical input, identical frames.
bool vg_replay_timed(void);

// Print the last timed run's cost again. False when there has not been one. Serial 'c'.
bool vg_replay_report_cost(void);

// One frame's raster cost, handed over by the frame loop while a timed replay runs.
// Ignored otherwise. Summed and reported when the replay ends.
void vg_replay_note_cost(uint32_t can, uint32_t rast, uint32_t prim,
                         uint32_t sub, uint32_t upd);

// The same frame's `world` split -- see g_w_motes in vg_prof.h. Separate from the call
// above because it answers a different question: that one is "what does a frame cost",
// this one is "which of the things in it grows when the fight gets busy", and the second
// is the one that needs a session with a fight in it.
void vg_replay_note_world(uint32_t motes, uint32_t rocks, uint32_t trails,
                          uint32_t ships, uint32_t msl, uint32_t fire,
                          uint32_t total);

// The same frame's BLIT split, kept the same way and reported on its own line.
//
// `blit` is join + wait + rast + push + res by construction, and only one of those is
// work: `rast` is the bands being drawn, and `push` is the CPU STOPPED because the SPI
// queue is full. A frame that spends its blit stalled has idle cores under a full wire,
// and a frame that spends it rastering does not -- which is the difference between
// submission being movable into the blit and it not being.
//
// `over` is the part that actually reaches the frame: a band under its 768 us window
// costs nothing at all, and the amount by which one goes OVER is frame time. Carried as
// bands and microseconds because the two say different things -- one heavy band in every
// few frames means something the mean of a ratio cannot.
void vg_replay_note_blit(uint32_t join, uint32_t wait, uint32_t push,
                         uint32_t res, uint32_t over_n, uint32_t over_us,
                         uint32_t sky, uint32_t scan);

// EVERY BAND'S OWN MICROSECONDS, meaned over the session.
//
// The counters above say how much frame time the raster lost; this says WHERE. A band is
// a strip of the screen, so a band over its window names a region of the picture that is
// too expensive -- which is an art brief, and a far more useful one than "use less area".
void vg_replay_note_bands(const uint32_t* band_us, int n);

// Core 0's measured idle for the same frame, microseconds. See the hook in main.cpp.
void vg_replay_note_idle0(uint32_t us);

// The primitive raster split by TYPE, for the frame's largest stage. `aa` is every
// blended line -- the trails live there -- and it took three wasted flights to admit
// the replay should have carried it from the day the type counters existed.
void vg_replay_note_types(uint32_t aa, uint32_t ln, uint32_t tri,
                          uint32_t gl, uint32_t fl);

// The SUBMIT split. `sub` is the one stage of the frame never broken down, and it is now
// the largest thing left: the raster is spent and this is not.
//
// `wait` is core 1 standing at the rendezvous while core 0 finishes group B. It is the
// number that says whether the two halves are balanced, and nothing measured it before.
void vg_replay_note_sub(uint32_t a, uint32_t b, uint32_t wait,
                        uint32_t arena, uint32_t star, uint32_t hud);

// PLAY: block for the next frame's record. False when the host says the
// session is finished.
bool vg_replay_next(float* dt, VgInput* in);

// RECORD: emit this frame's record. Call immediately after vg_game_update so
// that any seeds drawn during the step are attributed to the right frame.
void vg_replay_note_frame(float dt, const VgInput* in);

// True while a replay must not write progress to flash.
bool vg_replay_suppress_save(void);
