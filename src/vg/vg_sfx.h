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
    SFX_PANEL_ON,        // one region of the cockpit latching on
    SFX_SPOOL,           // the wind-up in the dark, before any of it
    SFX_IFT,             // the broadcast, about to say something
    SFX_IFT_SHORT,       // ...and continuing to
    SFX_DEATH_STATIC,    // the signal going, before the flatline settles
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

// The sound of somebody being dead -- the opponent over the radio, or the player
// over their own wreck. Held, not fired: on the wreck screen it lasts until the
// player taps away.
void vg_sfx_flatline(bool on);

// Cut every sound the ship is making, immediately.
//
// For the set going off. The transition is the end of a session, and a session
// that ends with a boundary warning still beeping over the black has not ended.
// The held sounds do not fade here: a fade is what you want when a ship stops
// flying, and this is the picture being taken away.
void vg_sfx_silence(void);

// Generate and hand over whatever the output will take. Called once a frame, and
// deliberately not from a task: the mixer is cheap, and a frame that is late has
// bigger problems than a gap in the audio.
//
// `dt` is the frame's simulated length, and it matters only while a replay is
// rendering. Live, the mixer follows the clock -- 22050 samples a second, however
// long the frame took. Rendering, the frames come as fast as the link allows and
// wall time means nothing, so it follows the SIMULATION instead and produces
// exactly one frame's worth. Otherwise the captured audio is stretched against
// its own picture by whatever the link happened to be doing.
void vg_sfx_update(float dt);

// THE MIX. Two settings, saved and adjustable, and they belong to the audio module
// rather than to VgGame -- which is where they were, because until now the struct
// every file includes was the only place a module could put state other files read.
//
// Kept whether or not anything is listening yet. The board has an ES8311 and a
// speaker; having the settings saved and adjustable first means sound arrives as a
// mix rather than as a feature that then needs a menu built around it.
//
// `music` HAS NO CONSUMER YET, and that is worth saying plainly rather than leaving
// to be discovered: it is set at boot, moved by the pause slider, drawn on that
// screen and written to flash, and nothing in the mixer reads it. `sfx` is live.
//
// NOT ZEROED BY ANY CLEAR, unlike the course and the transition. vg_game_init
// assigns both outright a few lines after its memset, so these never depended on
// being covered by it and nothing has to be added to keep a recording honest.
struct VgVolume {
    float music;
    float sfx;
};

extern VgVolume vg_vol;
