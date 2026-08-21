#pragma once
#include "vg_game.h"
#include "vg_voice.h"

// ===========================================================================
// THE BROADCAST: EVERY VOICE IN THE GAME, ON CHANNELS
//
// Two speakers exist today and a third is coming, and they were three unrelated blocks of
// fields on VgGame with three unrelated sets of rules. This is the one system, and adding a
// fourth voice is an enum entry and a table row rather than a new block of state and a new
// set of habits.
//
// WHAT IS A CHANNEL AND WHAT IS NOT. A channel is a VOICE -- something that speaks to the
// player from outside the cockpit. The missile banner is not one and used to sit here: it
// is the ship telling its own pilot what happened to a round, which is an instrument
// reading and belongs with the panel. Author's call, and the right one -- it is emitted by
// the HUD, not received by it.
//
// THE CHANNELS DO NOT SHARE A SLOT, AND THAT IS DELIBERATE. Sharing was the obvious thing
// and it breaks at the one moment that matters: after a kill the dying pilot holds his line
// for KILL_SPEECH, which is exactly when the broadcast is supposed to be summing up. Two
// slots let them overlap on purpose, and a loser still talking under the summary is the
// tone of the whole tournament. A single arbitrated queue would delete that, which is why
// this is channels and not a priority queue.
// ===========================================================================

// ---------------------------------------------------------------------------
// The channels
//
// ADDING ONE IS AN ENUM ENTRY AND A SPEC ROW. Keep BC_CHANNELS last; it is the count.
// ---------------------------------------------------------------------------
enum BcastChan : uint8_t {
    // The pilots, to each other. Arbitrated: a death cry is never stepped on by the next
    // round going wide.
    BC_PILOT = 0,
    // The broadcast's own voice -- between fights, never during one. It never interrupts
    // itself, so a later line simply replaces an earlier one and there is no priority.
    BC_IFT,
    // The interface describing the match itself: who is flying, what this round is. NOT the
    // broadcast talking about the match -- that is BC_IFT -- but the game stating what is
    // in front of the player. Reserved; the mechanism is here and the lines are the
    // author's.
    BC_MATCH,
    BC_CHANNELS
};

// One channel's current line. Uniform across channels on purpose: a reader that can draw
// one can draw any of them, and a new voice inherits a drawer for free.
struct BcastSlot {
    const char* line;     // null when the channel is quiet
    float       t;        // seconds remaining; <= 0 is quiet
    float       since;    // seconds since this channel last OPENED a line, for run detection
    char        tag[4];   // the speaker's badge, "" for a channel that does not badge
    uint8_t     pri;      // meaningless unless the spec arbitrates
    bool        mark;     // this line opens a run, so it is badged
};

// What makes one channel behave differently from another. All const data -- a channel is a
// row plus whatever posts to it, which is the shape vg_event.h proved at two instances.
struct BcastSpec {
    // Seconds a line holds when the caller does not say otherwise.
    float hold;
    // PRIORITY DECIDES, or a later line simply replaces the one up. The pilots arbitrate
    // because two of them talk at once; the broadcast does not because it is one speaker
    // who waits his turn by nature.
    bool  arbitrate;
    // Whether the first line of a run gets the speaker's badge. A continuation does not --
    // see the note on draw_comms.
    bool  badge;
};

struct Broadcast {
    BcastSlot ch[BC_CHANNELS];

    // ONE BIT PER IftSlot, so each cue fires once per match rather than every frame its
    // condition is true. On the system rather than the channel because the CALLERS own it:
    // vg_states decides when a cue has come true and sets the bit, and vg_overlay reads it
    // to know whether the intro has already been said. The composer never touches it.
    uint8_t ift_fired;
};

extern Broadcast vg_bcast;
extern const BcastSpec vg_bcast_spec[BC_CHANNELS];

// ---------------------------------------------------------------------------
// Posting
// ---------------------------------------------------------------------------

// Put a line on a channel. `hold` of 0 takes the channel's default; `pri` is ignored unless
// the channel arbitrates. Returns false if the channel was busy with something louder.
bool vg_bcast_post(BcastChan c, const char* line, const char* tag,
                   float hold, uint8_t pri);

// Silence one channel, or all of them. For a transition that makes a queue meaningless --
// nobody wants the last match's announcement finishing over the next one's opening.
void vg_bcast_clear_all(void);

// Advance every channel by dt. One call; channels do not each need driving.
void vg_bcast_step(float dt);

// Is this channel saying anything right now.

// ---------------------------------------------------------------------------
// The voices that post to it
// ---------------------------------------------------------------------------

// A pilot speaks, chosen from that ship's archetype. See vg_voice.h -- the LINES are the
// game's writing and the archetype decides which of them this pilot would say.
void vg_comms_say(const Ship* s, VoiceEvent ev);

// The broadcast speaks. Composed in vg_ift.cpp, which owns the slot table and the
// once-per-match bookkeeping; this is only the transport.
void vg_ift_say(const char* line, float hold, bool badge);
void vg_ift_queue(const char* line, float hold);
bool vg_ift_busy(void);
// How many lines of the current announcement have been read. An INT -- it is a count, not a
// fraction, and "progress" reads like the latter.
int vg_ift_progress(void);
void vg_ift_clear(void);

void vg_comms_step(float dt);
