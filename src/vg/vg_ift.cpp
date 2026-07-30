#include "vg_ift.h"
#include "vg_sim.h"
#include "vg_tourney.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// THE LINES BELOW ARE PLACEHOLDERS AND ARE NOT WRITING.
//
// They are data labels -- a callsign and a ship class, which is a scoreboard,
// not a script. The game's writing belongs to the author (see CLAUDE.md), so the
// mechanism is wired and the voice is left empty on purpose.
//
// To write the real lines, replace the format strings and nothing else. Each slot
// documents exactly which specifiers it gets and in what order. Keep the count
// and the order; the arguments are passed positionally.
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
// A line is drawn at scale 2 across the full width, so about 34 characters fit
// before it runs into the edges. Longer is not clipped, it is centred and will
// overhang.
// ---------------------------------------------------------------------------

static const char* const IFT_FMT[IFT_SLOTS] = {
    "%s   %s",              // IFT_INTRO_YOU
    "%s   %s   %d",         // IFT_INTRO_OPP
    "%s   %s   %s",         // IFT_MATCH_END
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

    default: return;
    }

    vg_ift_say(s_buf);
}
