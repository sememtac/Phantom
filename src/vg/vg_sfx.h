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

// Fire a cue. `pitch` scales the whole thing, 1.0 being its natural voice.
//
// THE WARNINGS IGNORE IT. An annunciator that changed pitch with range would be
// telling the pilot two things with one sound, and the second one badly -- real
// aircraft warnings are fixed, identical every time, so that recognising one
// costs nothing and hearing it wrong is impossible. The alerts pass 1.0 and the
// argument stays for the cues that are sounds rather than instruments.
void vg_sfx_play(SfxId id, float pitch);

// The airframe, which is not a cue: it is on for as long as the ship is flying
// and follows the throttle. Called every frame with the current setting; `on`
// false lets it fall silent rather than cutting, so leaving a match does not
// clip the hum off mid-cycle.
void vg_sfx_engine(bool on, float throttle);

// Generate and hand over whatever the output will take. Called once a frame, and
// deliberately not from a task: the mixer is cheap, and a frame that is late has
// bigger problems than a gap in the audio.
void vg_sfx_update(void);
