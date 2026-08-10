#pragma once

// ===========================================================================
// THE IFT: the broadcast voice
//
// The fiction is that a tournament round is televised, so there is a voice that
// is not a pilot. It introduces the two fighters over the launch cutscene and it
// sums up the round once the loser has finished talking.
//
// It is WHITE, and that is a structural choice rather than a preference. Hue in
// this game means identity: every ship earns one, and vg_tourney spreads fifteen
// of them while deliberately keeping clear of the player's own trail colour. The
// IFT is not a competitor. A hue would make it a sixteenth entrant. White is the
// absence of a hue rather than another one, so it can never be mistaken for a
// ship and it costs nothing from the wheel.
//
// It has its own comms slot. Sharing the pilots' slot fails at the one moment
// that matters -- after a kill the dying pilot owns that slot for KILL_SPEECH,
// which is exactly when the summary is due -- and two slots let them overlap on
// purpose. A loser still talking under the broadcast summing up is the tone of
// the whole tournament.
//
// IT DOES NOT SPEAK DURING A FIGHT. Mid-match it would compete with pilot
// chatter and with the missile and boundary alerts, and silence while the
// shooting happens is what makes the broadcast feel like it cut to the action.
// Other mechanics may earn it a voice later; these three moments are the floor.
//
// THE ANOMALY IS THAT MECHANIC, and it is the first thing to earn the broadcast a
// voice mid-fight. The exception is narrow and it is the reason the rule held: the
// arena itself changing shape is information the player cannot get anywhere else,
// and it cannot be read off the instruments. A pilot would not say it -- they are
// busy, and they are not the authority on the venue. So the broadcast says it, in
// the one register it has left, which is that it does not know either.
// ===========================================================================

enum IftSlot : unsigned char {
    IFT_INTRO_YOU = 0,   // over the player's shot in the cutscene
    IFT_INTRO_OPP,       // over the opponent's shot
    IFT_MATCH_END,       // after the loser's last transmission
    IFT_COURSE_START,    // the ring course, which the IFT runs
    IFT_COURSE_PASS,
    IFT_COURSE_MISS,
    IFT_COURSE_DONE,
    IFT_ANOMALY,         // the arena has started to distort, mid-fight
    IFT_ANOMALY_END,     // ...and it is passing
    IFT_SLOTS
};

// Compose and post the line for a slot. Safe to call every frame; the state
// machine fires each slot once per match.
void vg_ift_line(IftSlot slot);
