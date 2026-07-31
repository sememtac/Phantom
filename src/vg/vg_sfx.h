#pragma once

// ===========================================================================
// SOUND
//
// GENERATED, not played. Every cue here is a few oscillators and an envelope,
// which is not a compromise forced by the 2.8MB of free flash -- twenty short
// samples would have fitted easily. It is the right instrument for the machine:
// a wireframe dogfight scored by square waves and filtered noise is coherent in
// a way a compressed recording of a real explosion would not be, and the whole
// synth costs less flash than one second of PCM.
//
// It also means a cue can respond. A missile alert that rises in pitch as the
// range closes is one line here and a directory of variants otherwise.
//
// AUDIO NEVER TOUCHES THE SIMULATION. It draws no random numbers from the game's
// seeded stream, sets no state the game reads, and takes no decision. Replay
// determinism depends on the sim being a pure function of (seed, dt, input), and
// a sound that could nudge any of those would take the capture tool down with it.
// ===========================================================================

enum SfxId : unsigned char {
    SFX_MSL_ALERT = 0,   // incoming missile -- the annunciator's own voice
    SFX_WALL_ALERT,      // boundary
    SFX_MSL_EVENT,       // a round resolved: hit, miss, kill
    SFX_COMMS,           // somebody is on the radio
    SFX_LAUNCH,          // one of ours leaves the rail
    SFX_EXPLODE,
    SFX_COUNT
};

bool vg_sfx_init(void);

// Fire a cue. `pitch` scales the whole thing, 1.0 being its natural voice --
// used by the alerts, which climb as the thing they are warning about closes.
void vg_sfx_play(SfxId id, float pitch);

// Generate and hand over whatever the output will take. Called once a frame, and
// deliberately not from a task: the mixer is cheap, and a frame that is late has
// bigger problems than a gap in the audio.
void vg_sfx_update(void);
