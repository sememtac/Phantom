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
// is, and it goes to the same generator and the same mixer. But it goes to the
// music's OWN voices: vg_synth keeps eight for the score, apart from the ten the
// cues use, and neither side can take from the other. It used to be one pool of
// ten with no priority, and this player stopped adding notes near the top of it
// so that an alert always had a voice. In a fight the pool was near the top most
// of the time, and the music was what went missing. Now every note is posted,
// and the eight cover the thickest bar any score has, which is seven.
//
// NOTHING HERE TOUCHES THE SYNTH DIRECTLY EITHER. Notes are posted through
// vg_sfx_note and applied by whichever side is rendering, because on the board
// the audio task owns the voices and this runs on the game thread.
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
