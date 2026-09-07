#pragma once
#include "vg_synth.h"

// ===========================================================================
// THE SCORE PLAYER
//
// Music, which the game had none of until now. A score is a table baked by
// tools/score.py from a file in design/score/: one row for each different note,
// and one step for each time a note sounds. This walks the steps against a
// clock and hands each one to vg_synth, the same generator every cue uses.
//
// IT IS NOT A SECOND SYNTHESISER. A note here is a SynthLayer, exactly as a cue
// is, so music and sound effects share one voice pool and one mixer. That is
// also the constraint: there are 10 voices and no priority, and vg_synth takes
// the voice with the least time left. Music that used every voice would starve
// the missile alert, so the player STOPS ADDING NOTES near the top of the pool
// and lets the cues have the rest. A dropped note is a smaller fault than an
// alert that did not sound.
//
// NOTHING HERE TOUCHES THE SIMULATION. It draws no random numbers, reads no
// game state and sets none. It is stepped with the same dt as vg_sfx_update, so
// a replay drives it from simulated time like everything else and the capture
// stays frame for frame.
// ===========================================================================

#ifndef VG_SCORE_STEP
#define VG_SCORE_STEP
// One note that plays, at t_ms from the start of the score.
//
// t_ms IS 32 BIT and must stay that way. It was 16, which stops at 65.5 seconds,
// and a 96 second theme wrapped round and played its second half over its first.
// The extra two bytes for each step cost a few kilobytes of flash, which this
// board has and does not miss.
struct VgScoreStep { uint32_t t_ms; uint8_t note; };
#endif

// Above this many live voices the player adds nothing, and the cues keep what is
// left. Seven of ten is the whole music at its thickest, so the guard is a floor
// under the alerts rather than a limit the music normally meets.
#define VG_SCORE_VOICES 8

// Start a score. `steps` must be sorted by t_ms, which the baker guarantees.
void vg_score_play(const VgScoreStep* steps, int n_steps,
                   const SynthLayer* notes, int len_ms, bool loop);

// Stop it. Held voices are left to finish; use vg_sfx_silence to cut them.
void vg_score_stop(void);

// Hold the music where it is, and let it go on again from there. NOT stop and
// play: play starts a score from its beginning, and a pause has to give back the
// bar it took. Notes already sounding are left to finish rather than cut, which
// is a few hundred milliseconds and reads as the music being held rather than
// switched off.
void vg_score_pause(bool held);

bool vg_score_playing(void);

// One frame. Give it the same dt as vg_sfx_update.
void vg_score_update(float dt);
