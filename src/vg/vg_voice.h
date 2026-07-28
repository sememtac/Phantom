#pragma once
#include <stdint.h>

// ===========================================================================
// PILOT VOICES
//
// The bracket generates fifteen strangers with three-letter tags. On its own
// that is a spreadsheet. What makes them opponents rather than entries is that
// they talk -- and specifically that they talk AT you, at the four moments you
// are already paying attention: when they size you up, when they shoot, when
// you hurt them, and when you kill them.
//
// The death lines carry the setting. The IFT runs this for viewing figures and
// tax revenue, and a game that says so in its opening crawl and then has pilots
// politely explode is not telling the truth about itself. So they die badly,
// and they die on the radio where you have to listen to it.
//
// Lines are capped at 26 characters. That is what fits across the panel at
// readable size beside a callsign tag, and the limit does the writing a favour:
// radio chatter under fire is clipped, not composed.
// ===========================================================================

enum VoiceEvent : uint8_t {
    VOICE_TAUNT = 0,   // sizing you up, between passes
    VOICE_FIRE,        // has just launched
    VOICE_HURT,        // took a hit and lived
    VOICE_DEATH,       // did not
    VOICE_EVENTS
};

#define VOICE_LINES 3

struct PilotVoice {
    const char* archetype;
    const char* line[VOICE_EVENTS][VOICE_LINES];
};

extern const PilotVoice vg_pilot_voice[];

// How many archetypes a generated rival may be given. The legend's own voice
// sits past this and is never rolled -- there is only ever one Phantom, and it
// is either the entrant waiting in the final or it is you.
int vg_voice_rollable(void);
int vg_voice_phantom(void);

// Once the player has taken a tournament the rivals know who they are up
// against, and some of them say so. Mixed into taunts only -- a pilot bleeding
// out does not stop to admire your reputation.
const char* vg_voice_champion_line(uint32_t pick);

// `pick` is any changing value; it is reduced modulo the line count, so callers
// can pass a frame counter or a random draw without caring about the range.
const char* vg_voice_line(uint8_t voice, VoiceEvent ev, uint32_t pick);
const char* vg_voice_archetype(uint8_t voice);
