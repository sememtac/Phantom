#include "vg_sim.h"
#include "vg_comms.h"
#include "vg_sfx.h"
#include "vg_tourney.h"
#include <stdio.h>
#include <string.h>

// The two radio channels, and the missile banner that shares their timing.
//
// One is the pilots talking to each other and one is the broadcast, and they are
// separate on purpose -- the note on the IFT below says why. What makes them one
// module rather than two is that both are QUEUES WITH PRIORITY served by a single
// tick, and every bug either of them has ever had was about what displaces what.


Broadcast vg_bcast;

// ONE ROW PER CHANNEL, in BcastChan order. A new voice is a row here and an entry in the
// enum -- which is the shape vg_event.h proved at two instances and the reason this is a
// table rather than three sets of habits.
const BcastSpec vg_bcast_spec[BC_CHANNELS] = {
    // hold   arbitrate  badge
    {  2.4f,  true,      true  },   // BC_PILOT -- two of them talk at once, so priority decides
    {  0.0f,  false,     true  },   // BC_IFT   -- one speaker who waits his turn; callers set the hold
    {  3.0f,  false,     true  },   // BC_MATCH -- reserved; the mechanism only
};

// THE GENERIC HALF, AND IT IS DELIBERATELY THIN.
//
// Arbitration, the slot, and the clock. What it does NOT do is decide whether a line opens
// a run: that rule is different for every voice -- a pilot's last transmission is never a
// continuation however fast it followed the last one -- and generalising it is exactly how
// "the callsign never appears again after the first" comes back. Callers decide `mark` and
// hand it in.
bool vg_bcast_post(BcastChan c, const char* line, const char* tag, float hold, uint8_t pri) {
    if (c >= BC_CHANNELS || !line) return false;
    BcastSlot* sl = &vg_bcast.ch[c];
    const BcastSpec* sp = &vg_bcast_spec[c];

    // Strict '>' so an equal event still refreshes: two hits in a row should read as two.
    if (sp->arbitrate && sl->t > 0.0f && sl->pri > pri) return false;

    sl->line  = line;
    sl->t     = (hold > 0.0f) ? hold : sp->hold;
    sl->pri   = sp->arbitrate ? pri : 0;
    sl->since = 0.0f;
    if (tag) {
        sl->tag[0] = tag[0]; sl->tag[1] = tag[1]; sl->tag[2] = tag[2]; sl->tag[3] = 0;
    } else {
        sl->tag[0] = 0;
    }
    return true;
}

void vg_bcast_clear(BcastChan c) {
    if (c >= BC_CHANNELS) return;
    vg_bcast.ch[c] = BcastSlot{};
}

// Everything off the air. For vg_game_init, which begin_record restarts the game through --
// these slots relied on the memset of vg until they left it.
void vg_bcast_clear_all(void) {
    for (int i = 0; i < BC_CHANNELS; i++) vg_bcast.ch[i] = BcastSlot{};
    vg_bcast.ift_fired = 0;
}

bool vg_bcast_live(BcastChan c) {
    return c < BC_CHANNELS && vg_bcast.ch[c].line && vg_bcast.ch[c].t > 0.0f;
}

// Every channel's clock, in one place. `since` runs whether or not the channel is speaking,
// because it measures the gap BETWEEN lines and that is what run detection needs.
void vg_bcast_step(float dt) {
    for (int i = 0; i < BC_CHANNELS; i++) {
        BcastSlot* sl = &vg_bcast.ch[i];
        sl->since += dt;
        if (sl->t > 0.0f) {
            sl->t -= dt;
            if (sl->t <= 0.0f) { sl->line = nullptr; sl->pri = 0; }
        }
    }
}

void vg_comms_say(const Ship* s, VoiceEvent ev) {
    if (!s) return;
    // VoiceEvent is ordered by weight -- taunt, fire, hurt, death -- so the
    // enum value doubles as the priority and a strict '>' means an equal event
    // still refreshes. Two hits in a row should read as two.
    if (vg_bcast.ch[BC_PILOT].t > 0.0f && vg_bcast.ch[BC_PILOT].pri > (uint8_t)ev) return;

    const uint32_t pick = (uint32_t)(vg_frand01() * 997.0f);
    const char* line = nullptr;

    // Once the name is yours, rivals sometimes stop taunting and start
    // recognising. Taunts only -- a pilot bleeding out does not pause to admire
    // your reputation, and their own last words matter more than your legend.
    if (ev == VOICE_TAUNT && vg.champion && (pick & 1u))
        line = vg_voice_champion_line(pick >> 1);
    else
        line = vg_voice_line(s->voice, ev, pick);
    // Badge this line unless it is genuinely CARRYING ON from the last one.
    //
    // The test used to be "is the previous line still on screen", and a pilot's
    // line stays up for 2.4 seconds while a pilot in a fight speaks far more
    // often than that -- so nearly every transmission counted as a continuation
    // and the callsign effectively never appeared again after the first. The
    // rule was right and the threshold was somebody else's.
    //
    // Continuing means immediately. Anything after a beat is a new statement,
    // and a new statement says who is making it.
    //
    // A LAST TRANSMISSION IS NEVER A CONTINUATION. A pilot almost always takes
    // hits on the way down, and a salvo puts those hurt lines well inside the
    // threshold -- so the one line in the match that most needs a name on it was
    // the one reliably losing it. It is not the end of a sentence somebody was
    // already saying. It is a different kind of statement, and it is the last
    // thing that pilot ever says.
    const bool same_voice = (vg_bcast.ch[BC_PILOT].tag[0] == s->tag[0]
                          && vg_bcast.ch[BC_PILOT].tag[1] == s->tag[1]
                          && vg_bcast.ch[BC_PILOT].tag[2] == s->tag[2]);
    // DECIDED HERE AND APPLIED AFTER THE POST. vg_bcast_post does the slot, the clock and
    // the arbitration; whether a line opens a run is pilot policy -- a last transmission
    // never continues anything -- and generalising it is how the callsign went missing.
    const bool opens = (ev == VOICE_DEATH)
                    || !(same_voice && vg_bcast.ch[BC_PILOT].since < 0.7f);
    // A last transmission is left up far longer. It is the only line the player
    // cannot provoke a second time, and the round is already decided -- there is
    // nothing it can be competing with.
    vg_bcast_post(BC_PILOT, line, s->tag,
                  (ev == VOICE_DEATH) ? KILL_SPEECH : 0.0f, (uint8_t)ev);
    vg_bcast.ch[BC_PILOT].mark = opens;

    // The radio opening, not the words. A last transmission gets a lower blip --
    // the only thing distinguishing it by ear, and it should not sound routine.
    vg_sfx_play(SFX_COMMS, (ev == VOICE_DEATH) ? 0.72f : 1.0f);
}

// The broadcast voice. White, and outside the hue system entirely.
//
// Every ship in the arena earns a hue, because hue means identity here and a
// trail is the one colour you never mistake. The IFT is not in the fight, so
// giving it a hue would make it a sixteenth entrant. White is the absence of one,
// which is what a broadcast layer should be -- and it costs nothing from the
// fifteen pilot hues that vg_tourney already has to spread and keep clear of the
// player's own.
//
// It can be given several lines at once and will read them in order. One line at
// a time was right while it only ever announced a result; it is wrong the moment
// it has to say something conversational, because a paragraph delivered as one
// overlong line is not the same performance as three beats.
#define IFT_QUEUE_MAX 4

static char  s_ift_q[IFT_QUEUE_MAX][48];
static float s_ift_hold[IFT_QUEUE_MAX];
static int   s_ift_n = 0;   // lines waiting
static int   s_ift_i = 0;   // how far through them we are
static float s_ift_gap = 0.0f;   // silence owed before the next line

static void ift_pop(bool opens_run) {
    if (s_ift_i >= s_ift_n) { s_ift_n = s_ift_i = 0; return; }
    vg_bcast.ch[BC_IFT].line = s_ift_q[s_ift_i];
    vg_bcast.ch[BC_IFT].t    = s_ift_hold[s_ift_i];
    vg_bcast.ch[BC_IFT].mark = opens_run;
    // EVERY line is announced, because every line is a system message. The
    // opener gets the full double beat and the lines continuing it get the short
    // form -- the distinction the badge draws visually, drawn again by ear.
    vg_sfx_play(opens_run ? SFX_IFT : SFX_IFT_SHORT, 1.0f);
    s_ift_i++;
}

// Off the air: the line showing, the gap owed, and everything still queued.
//
// The whole queue, not just the timer. Zeroing vg_bcast.ch[BC_IFT].t alone leaves the
// indices saying there is more to read, and the next caller to queue anything
// inherits the leftovers.
void vg_ift_clear(void) {
    vg_bcast.ch[BC_IFT].line = nullptr;
    vg_bcast.ch[BC_IFT].t    = 0.0f;
    s_ift_gap   = 0.0f;
    s_ift_n = s_ift_i = 0;
}

void vg_ift_say(const char* line, float hold, bool badge) {
    if (!line) return;
    s_ift_gap = 0.0f;
    // An immediate line cuts off anything queued. A broadcast that has moved on
    // must not have the tail of the last announcement surface behind it.
    s_ift_n = s_ift_i = 0;
    vg_bcast.ch[BC_IFT].line = line;
    vg_bcast.ch[BC_IFT].t    = hold;
    vg_bcast.ch[BC_IFT].mark = badge;
    // Same rule as the badge: an unbadged line is a caption on somebody else's
    // ship, and captions do not announce themselves.
    if (badge) vg_sfx_play(SFX_IFT, 1.0f);
}

// True while the broadcast is mid-announcement: a line up, a pause between two,
// or lines still waiting to be read. Callers use it to keep out of the way --
// anything that would post its own line has to wait, or it silently deletes the
// rest of what was being said.
int vg_ift_progress(void) { return s_ift_i; }

bool vg_ift_busy(void) {
    return (vg_bcast.ch[BC_IFT].line && vg_bcast.ch[BC_IFT].t > 0.0f) || s_ift_gap > 0.0f || s_ift_i < s_ift_n;
}

// Queued lines are COPIED. A caller composing each line in its own scratch
// buffer and queueing three of them would otherwise end up holding three
// pointers to the same buffer and hear the last line three times.
void vg_ift_queue(const char* line, float hold) {
    if (!line || !*line) return;

    // A SILENT CHANNEL WITH LINES STILL QUEUED means somebody zeroed the timer
    // out from under a run -- vg_match_start does exactly that -- and those
    // lines will never be read now. Discarding them here is what keeps the queue
    // honest without every such caller having to remember the indices exist.
    //
    // Leaving them cost more than a stale line: the next announcement appended
    // to the wreckage and popped from the MIDDLE of it, so the wrong line opened
    // the run and took the badge with it. That is how this was found.
    //
    // A PENDING GAP IS NOT A DEAD RUN. Mid-announcement the channel is silent by
    // design, and without this second test the gap between two lines would look
    // exactly like abandonment and throw away the rest of what was being said.
    if (vg_bcast.ch[BC_IFT].t <= 0.0f && s_ift_gap <= 0.0f && s_ift_i < s_ift_n)
        s_ift_n = s_ift_i = 0;

    if (s_ift_n >= IFT_QUEUE_MAX) return;   // silently, rather than shouting over
    snprintf(s_ift_q[s_ift_n], sizeof(s_ift_q[0]), "%s", line);
    s_ift_hold[s_ift_n] = hold;
    s_ift_n++;
    // Nothing up and nothing owed, so this one starts talking -- and starting is
    // what earns the badge.
    if (vg_bcast.ch[BC_IFT].t <= 0.0f && s_ift_gap <= 0.0f) ift_pop(true);
}

// Everything the radio is doing this frame: the pilot channel timing out, the
// broadcast channel timing out and pulling the next queued line, and the missile
// banner working through its own backlog.
//
// Called from the tail of vg_world_step, which is where the dt is. It sits HERE,
// next to the queue it drives and the statics it reads, so that when vg_comms.cpp
// happens this leaves with them rather than having to be found first.
//
// The banner is here rather than with the HUD timers because it is not a decay.
// It is a queue being served: when one banner's time runs out the next is
// promoted, and a salvo therefore reports itself out promptly instead of
// trailing the fight.
void vg_comms_step(float dt) {
    if (vg.msl_event_t > 0) {
        vg.msl_event_t -= dt;
        if (vg.msl_event_t <= 0) {
            if (vg.msl_qn > 0) {
                vg.msl_event = vg.msl_queue[0];
                for (int i = 1; i < vg.msl_qn; i++) vg.msl_queue[i - 1] = vg.msl_queue[i];
                vg.msl_qn--;
                // Held briefly when more are stacked up, so a salvo reports
                // itself out promptly instead of trailing the fight.
                vg.msl_event_t = vg.msl_qn ? MSL_BANNER_FAST : MSL_BANNER;
            } else {
                vg.msl_event = MSL_NONE;
            }
        }
    }

    // EVERY channel's clock, in one call. The IFT's own queue still needs its gap handled
    // below, which is the one thing the generic step cannot know about.
    const bool ift_was_up = vg_bcast.ch[BC_IFT].t > 0.0f;
    vg_bcast_step(dt);

    if (ift_was_up) {
        // Down, then a beat of silence before whatever is next -- so a queued
        // announcement reads as consecutive beats rather than one paragraph.
        if (vg_bcast.ch[BC_IFT].t <= 0) {
            vg_bcast.ch[BC_IFT].line = nullptr;
            s_ift_gap   = (s_ift_i < s_ift_n) ? IFT_GAP : 0.0f;
        }
    } else if (s_ift_gap > 0.0f) {
        s_ift_gap -= dt;
        // A continuation, so no badge: the channel is mid-announcement and nobody
        // else can have taken it.
        if (s_ift_gap <= 0.0f) ift_pop(false);
    }
}
