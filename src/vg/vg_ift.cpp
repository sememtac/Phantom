#include "vg_ift.h"
#include "vg_sim.h"
#include "vg_tourney.h"
#include "vg_course.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// SOME OF THESE LINES ARE THE AUTHOR'S WRITING. DO NOT EDIT THOSE.
//
// The course lines -- START and DONE -- are written, and belong to the author
// like everything else the player reads (see CLAUDE.md). Change them only when
// asked to, and never to make something fit.
//
// The rest are still PLACEHOLDERS and are not writing: they are data labels, a
// callsign and a ship class, which is a scoreboard rather than a script. Those
// are waiting for a voice.
//
// To write one, replace the format string and nothing else. Each slot documents
// exactly which specifiers it gets and in what order. Keep the count and the
// order; the arguments are passed positionally.
//
//   IFT_INTRO_YOU   %s  the player's callsign        (3 characters)
//                   %s  the player's ship class name
//
//   IFT_INTRO_OPP   %s  the opponent's callsign      (3 characters)
//                   %s  the opponent's ship class name
//                   %d  the opponent's rating, 0..100
//
//   IFT_MATCH_END   %s  the winner's callsign
//                   %s  the loser's callsign
//                   %s  the name of the round just finished
//
//   IFT_COURSE_START  no arguments
//   IFT_COURSE_PASS   %d  gates cleared in a row   %d  the target
//   IFT_COURSE_MISS   no arguments
//   IFT_COURSE_DONE   no arguments
//
// A line is drawn at SCALE 3, which fits about 22 characters beside the IFT mark
// or 26 without it -- continuation lines in a queued run carry no mark and get
// the extra room. Longer than that drops to scale 2 automatically rather than
// being clipped, so a written line is never truncated to protect a font size.
// ---------------------------------------------------------------------------

static const char* const IFT_FMT[IFT_SLOTS] = {
    "%s   %s",              // IFT_INTRO_YOU
    "%s   %s   %d",         // IFT_INTRO_OPP
    "%s   %s   %s",         // IFT_MATCH_END
    // IFT_COURSE_START is three lines, read in order, so it is composed below
    // rather than here. The table holds one line per slot by construction.
    "-",
    "%d / %d",              // IFT_COURSE_PASS
    "",                     // IFT_COURSE_MISS
    "CALIBRATION COMPLETE.", // IFT_COURSE_DONE
};

// One buffer, because only one line is ever up: the slot is single and a new line
// replaces the old one outright.
static char s_buf[80];

void vg_ift_line(IftSlot slot) {
    if (slot >= IFT_SLOTS) return;
    const char* fmt = IFT_FMT[slot];
    if (!fmt || !*fmt) return;          // an unwritten slot simply stays silent

    const Entrant* opp = vg_tourney_opponent();

    switch (slot) {
    case IFT_INTRO_YOU:
        snprintf(s_buf, sizeof(s_buf), fmt, vg.callsign, vg.spec->name);
        break;

    case IFT_INTRO_OPP: {
        if (!opp) return;
        // Rating is 0..1 internally. Reported as a percentage because a broadcast
        // quotes a number a viewer can hold, not a fraction.
        const int rating = (int)(opp->rating * 100.0f + 0.5f);
        snprintf(s_buf, sizeof(s_buf), fmt, opp->tag, vg_spec(opp->cls)->name, rating);
        break;
    }

    case IFT_MATCH_END: {
        // Reached only when the player won: a loss goes to VG_OVER, which the
        // broadcast concludes separately and which is not wired yet.
        if (!opp) return;
        snprintf(s_buf, sizeof(s_buf), fmt, vg.callsign, opp->tag,
                 vg_tourney_round_name(vt.round));
        break;
    }

    case IFT_COURSE_PASS:
        snprintf(s_buf, sizeof(s_buf), fmt, (int)vg.course_hits, COURSE_TARGET);
        break;

    // The course opening: three beats, not one line. The IFT reads the player in
    // before it hands over the assignment, which is the only place in the game
    // the broadcast addresses them directly rather than about them.
    case IFT_COURSE_START: {
        char welcome[48];
        snprintf(welcome, sizeof(welcome), "Welcome %s", vg.callsign);
        vg_ift_queue("Analyzing biometrics...", IFT_SPEECH_BEAT);
        vg_ift_queue(welcome,                   IFT_SPEECH_BEAT);
        vg_ift_queue("Complete this assignment to proceed", IFT_SPEECH);
        return;
    }

    // No arguments, so the author's line is the whole of it.
    case IFT_COURSE_MISS:
    case IFT_COURSE_DONE:
        snprintf(s_buf, sizeof(s_buf), "%s", fmt);
        break;

    default: return;
    }

    // The intro lines hold for less time, so the fighter's name card can follow
    // them within the same shot rather than fighting them for the same rows.
    const bool intro = (slot == IFT_INTRO_YOU || slot == IFT_INTRO_OPP);
    vg_ift_say(s_buf, intro ? IFT_SPEECH_INTRO : IFT_SPEECH);
}
