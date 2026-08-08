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

// One frame's raster cost, handed over by the frame loop while a timed replay runs.
// Ignored otherwise. Summed and reported when the replay ends.
void vg_replay_note_cost(uint32_t can, uint32_t rast, uint32_t prim,
                         uint32_t sub, uint32_t upd);

// PLAY: block for the next frame's record. False when the host says the
// session is finished.
bool vg_replay_next(float* dt, VgInput* in);

// RECORD: emit this frame's record. Call immediately after vg_game_update so
// that any seeds drawn during the step are attributed to the right frame.
void vg_replay_note_frame(float dt, const VgInput* in);

// True while a replay must not write progress to flash.
bool vg_replay_suppress_save(void);
