#pragma once
#include "vg_game.h"
#include "vg_voice.h"

// ===========================================================================
// THE PILOT CHANNEL AND THE BROADCAST
//
// Two speakers with one rule between them: only one line is up at a time. The
// pilots talk to each other, the broadcast talks over both, and the queue exists
// so a transmission that arrives mid-line is not simply lost.
//
// THE BROADCAST SLOT IS RESERVED, and this header does not settle it. msl_event,
// msl_queue, msl_qn, comms and the ift_* fields are still on VgGame, and the author
// has said the broadcast design gets its own pass. Worth restating why it is not a
// simple move: the slot is written by vg_comms.cpp and vg_ift.cpp -- the module it
// is named after -- never touches those fields. Who owns it is a design question
// with two defensible answers.
//
// So this file declares the FUNCTIONS and deliberately publishes no state.
// ===========================================================================

// Put one of this pilot's lines on the radio. Higher-priority events displace
// lower ones and never the other way round, so a death is always heard out.
void vg_comms_say(const Ship* s, VoiceEvent ev);
// The broadcast voice: white, its own slot, silent during a fight.
// `badge` puts the IFT mark on the line. Off for anything that is ABOUT somebody
// else -- a fighter's name and class read as a label on that fighter, and a
// callsign block in front of it looks like whose fighter it is.
void vg_ift_say(const char* line, float hold, bool badge = true);
// Add a line to the end of the broadcast's queue, to be read after whatever is
// already up. The text is copied, so a caller's scratch buffer is safe to reuse.
void vg_ift_queue(const char* line, float hold);
// True while the broadcast is still saying something, including the silences
// inside a multi-line announcement.
bool vg_ift_busy(void);

// Lines of the current queue already begun, for pacing decisions.
int  vg_ift_progress(void);

// One frame of both radio channels and the missile banner queue. Called from the
// tail of vg_world_step because that is where the dt is.
void vg_comms_step(float dt);

// Silence the broadcast and drop anything queued behind it. For a transition
// that makes the queue meaningless -- nobody wants the last match's announcement
// finishing itself over the next one's opening.
void vg_ift_clear(void);
