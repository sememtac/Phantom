#pragma once

// ===========================================================================
// SOUND
//
// GENERATED, not played. The catalogue of cues is a table in vg_sfx.cpp and the
// machine that renders them is vg_synth.cpp -- what the sounds ARE, kept apart
// from how sound is MADE.
//
// Nothing is sampled. That is not a compromise forced by flash, since 2.8MB is
// free and twenty short samples would have fitted easily. It is the right
// instrument for the machine: a wireframe dogfight scored by square waves and
// filtered noise is coherent in a way a recording would not be, the whole synth
// costs less flash than one second of PCM, and a generated cue can respond --
// the missile alert climbs, a dying pilot's blip is pitched lower than a routine
// one. One line each here; a directory of variants if these were files.
// ===========================================================================

enum SfxId : unsigned char {
    SFX_MSL_ALERT = 0,   // incoming missile -- the annunciator's own voice
    SFX_WALL_ALERT,      // boundary
    SFX_MSL_EVENT,       // a round resolved: hit, miss, kill
    SFX_COMMS,           // somebody is on the radio
    SFX_LAUNCH,          // one of ours leaves the rail
    SFX_HIT,             // the player's hull takes it
    SFX_EXPLODE,
    SFX_TV_ON,           // the set finding the signal
    SFX_TV_OFF,          // ...and losing it
    SFX_READY,           // the cockpit coming up: systems online
    SFX_IFT,             // the broadcast, about to say something
    SFX_IFT_SHORT,       // ...and continuing to
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
