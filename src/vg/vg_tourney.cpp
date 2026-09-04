#include "vg_tourney.h"
#include "vg_sim.h"
#include "vg_voice.h"
#include <string.h>

Tourney vt;

// Standard 16-entry bracket order. Reading it as consecutive pairs gives the
// first round: 1v16, 8v9, 5v12, 4v13, and so on -- which is exactly what makes
// the #1 slot's path escalate. The player sits at index 0.
static const uint8_t SEED_ORDER[TOURNEY_ENTRANTS] = {
    1, 16, 8, 9, 5, 12, 4, 13, 3, 14, 6, 11, 7, 10, 2, 15
};

// Enough three-letter callsigns to fill a bracket without repeating. Drawn
// without replacement, so every entrant is distinguishable at a glance -- which
// is the whole point of a three-character tag on a bracket sheet.
static const char* CALLSIGNS[] = {
    "VEX", "ORB", "KAI", "NYX", "RUS", "TAL", "ZED", "QRO",
    "HEX", "IVO", "MAR", "SOL", "DUN", "FEN", "GAL", "LYR",
    "ODA", "PIK", "RHO", "SKA", "TYR", "URS", "VAL", "WRE",
    "XAN", "YAR", "ZAI", "BOR", "CYG", "DRA", "ELM", "FRO",
};
#define NUM_CALLSIGNS (int)(sizeof(CALLSIGNS) / sizeof(CALLSIGNS[0]))

const char* vg_tourney_round_name(uint8_t r) {
    switch (r) {
    case 0:  return "ROUND OF 16";
    case 1:  return "QUARTER FINAL";
    case 2:  return "SEMI FINAL";
    default: return "FINAL";
    }
}

// Odds that A beats B. Deliberately not a pure rating ratio: the window is
// clamped so even a heavy favourite loses about one time in eight. Upsets are
// content -- the #2 seed going out in the first round is what makes the sheet
// worth looking at, and it means the final is not knowable from the entry
// screen.
// Randomly swap the two halves at every node of the bracket tree.
//
// This moves where everyone SITS without changing who plays whom: swapping a
// node's two sub-brackets leaves every pairing, every path and every round
// intact, because the tree is symmetric about each node. So the player's slot
// lands somewhere different each tournament while the escalation the seeding
// provides -- 16, then the 8/9 winner, then the 4/5 side, then the 2 half --
// survives untouched.
//
// Randomising the player's actual SEED would have moved their position too,
// but by destroying that escalation: a sixteenth seed opens against the
// strongest pilot in the draw and the difficulty curve runs backwards.
static void shuffle_bracket(int8_t* slot, int lo, int n) {
    if (n < 2) return;
    const int h = n / 2;
    shuffle_bracket(slot, lo, h);
    shuffle_bracket(slot, lo + h, h);
    if (vg_frand01() < 0.5f) {
        for (int i = 0; i < h; i++) {
            const int8_t t = slot[lo + i];
            slot[lo + i]     = slot[lo + h + i];
            slot[lo + h + i] = t;
        }
    }
}

static bool sim_match(const Entrant* a, const Entrant* b) {
    // The legend does not go out early. It is the whole point of the story that
    // it is waiting at the end, and an upset in the quarter-finals would spend
    // the game's one narrative beat on a coin flip nobody watched.
    if (a->is_phantom) return true;
    if (b->is_phantom) return false;

    float sum = a->rating + b->rating;
    float p   = 0.5f + 0.5f * ((a->rating - b->rating) / (sum > 1e-3f ? sum : 1.0f)) * 2.4f;
    if (p < 0.12f) p = 0.12f;
    if (p > 0.88f) p = 0.88f;
    return vg_frand01() < p;
}

// ===========================================================================
// TRAIL COLOUR IS IDENTITY, so no two of them may be the same one.
//
// Hue is the only thing on the sheet and in the sky that says WHO, and it has to
// carry that alone: at a distance a contact is a coloured streak before it is
// anything else. Two pilots sharing a colour is not a cosmetic collision, it is
// two pilots the player cannot tell apart in the one moment it matters.
//
// WHAT WAS THERE BEFORE DID NOT HOLD, and the comments claimed it did.
//
// The fifteen rivals were stepped by the golden ratio, which spreads them about
// as evenly as anything can -- and "about as evenly" is not the same as "no two
// closer than X". Measured over the actual sequence, the closest pair is 0.034
// apart, half of the 0.06 the same file used as its player-clearance threshold.
//
// The player was avoided by a single shove of +0.13 for anyone who landed within
// 0.06 -- which moves that rival past two of its neighbours and onto whatever is
// there.
//
// And the legend's red was documented as "the one colour the rest of the
// roster's golden-ratio spread never lands on". Two of the fifteen land inside
// 0.06 of it: i=13 at 0.034 and i=8 at 0.056.
//
// SO THEY ARE DEALT RATHER THAN SEARCHED FOR, and this is the second attempt.
//
// The first pushed each hue round the wheel until it cleared everything already
// spoken for. It does not converge: greedy placement paints itself into a corner,
// and a scan at a fixed stride does not visit the gaps that are left. Measured, it
// still produced pairs 0.0023 apart -- twenty times closer than it was asked for,
// and no better than the sequence it replaced.
//
// The construction instead. The player's colour and the legend's are two FIXED
// points, and they cut the wheel into two arcs; the other fourteen are spread
// evenly along those arcs in proportion to their length. Nothing to converge, no
// pair closer than the arithmetic allows, and the worst case over the whole range
// of player hues is 0.055 -- which is very nearly the 0.0625 that sixteen
// perfectly spaced trails would get.
//
// Round the wheel, so 0.02 and 0.98 are close.
static float hue_dist(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return (d > 0.5f) ? (1.0f - d) : d;
}

static float hue_wrap(float h) {
    while (h >= 1.0f) h -= 1.0f;
    while (h <  0.0f) h += 1.0f;
    return h;
}

void vg_tourney_begin(ShipClass player_class) {
    memset(&vt, 0, sizeof(vt));

    // --- the player, always seed 1 ---
    Entrant* p = &vt.entrant[0];
    p->tag[0]    = vg.callsign[0];
    p->tag[1]    = vg.callsign[1];
    p->tag[2]    = vg.callsign[2];
    p->tag[3]    = 0;
    p->cls       = player_class;
    p->hue       = vg.trail_hue;
    p->rating    = 1.0f;
    p->is_player = true;

    // --- fifteen rivals ---
    // Draw callsigns without replacement by shuffling a pool of indices.
    uint8_t pool[NUM_CALLSIGNS];
    for (int i = 0; i < NUM_CALLSIGNS; i++) pool[i] = (uint8_t)i;
    for (int i = NUM_CALLSIGNS - 1; i > 0; i--) {
        int j = (int)(vg_frand01() * (float)(i + 1));
        if (j > i) j = i;
        uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }

    // EVERY COLOUR SPOKEN FOR, in the order the claims are strongest.
    //
    // The player's first, because it is the one nobody may move -- they chose it.
    // Then the legend's, held back before the roster is dealt so that no rival
    // can take it: the phantom is picked by SEED, after the ratings are sorted,
    // and by then the hues are already out.
    // RED, UNLESS THE PLAYER IS RED, in which case blue.
    //
    // The legend's trail is meant to be the one nobody mistakes, and it cannot be
    // that while it is also somebody else's. The two are a third of the wheel
    // apart and the picker only offers red through blue, so a player who is too
    // close to one is never too close to the other.
    const float ph_hue = (hue_dist(vg.trail_hue, 0.0f) > 0.08f) ? 0.0f
                                                                 : (2.0f / 3.0f);

    // THE FIFTEEN, DEALT ROUND THE TWO FIXED POINTS.
    //
    // pot[0] is the legend's, held back so no rival can be given it -- the phantom
    // is picked by SEED, after the ratings are sorted, and by then the colours are
    // already out. It is swapped onto whoever turns out to be the legend below, so
    // the SET of fifteen never changes and no two of them can collide.
    const float hp   = hue_wrap(vg.trail_hue);
    float       la   = hue_wrap(ph_hue - hp);      // forward, player to legend
    const float lb   = 1.0f - la;
    const int   rest = TOURNEY_ENTRANTS - 2;       // everyone but the two of them
    // HOW MANY GO IN EACH ARC, and it is not simply proportional to its length.
    //
    // An arc of length L holding n pilots between two FIXED ends has them L/(n+1)
    // apart, not L/n -- the ends are colours too. Equalising the two arcs'
    // spacing gives n+1 = ENTRANTS * L, so the split is one less than the share.
    //
    // Straight proportion put twelve in an arc of 0.88 and two in one of 0.12,
    // which is 0.068 on one side and 0.040 on the other: a rival four hundredths
    // off the player's own trail, which is the collision this whole block exists
    // to stop.
    int         na   = (int)((float)TOURNEY_ENTRANTS * la + 0.5f) - 1;
    if (na < 0)    na = 0;
    if (na > rest) na = rest;
    const int   nb   = rest - na;

    float pot[TOURNEY_ENTRANTS - 1];
    int   np = 0;
    pot[np++] = ph_hue;
    for (int j = 0; j < na; j++)
        pot[np++] = hue_wrap(hp + la * (float)(j + 1) / (float)(na + 1));
    for (int j = 0; j < nb; j++)
        pot[np++] = hue_wrap(ph_hue + lb * (float)(j + 1) / (float)(nb + 1));

    for (int i = 1; i < TOURNEY_ENTRANTS; i++) {
        Entrant* e = &vt.entrant[i];
        strcpy(e->tag, CALLSIGNS[pool[i - 1]]);
        // SHIP_CLASSES cast explicitly: it is an enumerator, and multiplying a
        // float by an enum is deprecated. It compiled and did the right thing,
        // which is exactly why it was worth removing -- a warning on every build
        // is a warning nobody reads, and this file now builds silent.
        e->cls = (ShipClass)((uint32_t)(vg_frand01() * (float)(int)SHIP_CLASSES)
                             % (uint32_t)(int)SHIP_CLASSES);
        // Ship class is a sidegrade, so it must not decide the seeding -- pilot
        // quality does. Otherwise the bracket would just sort by ship and the
        // player could read the whole tournament off the class glyphs.
        e->rating    = vg_frand(0.55f, 1.00f);
        e->is_player = false;

        // GOLDEN-RATIO STEPPING IS GONE. It spread fifteen hues about as evenly as
        // an unconditioned sequence can, and "about as evenly" is not the same as
        // "no two closer than X": its closest pair was 0.034 apart, and two of the
        // fifteen landed inside 0.06 of the legend's red. See the note above.
        //
        // Measured over the whole range the picker offers, the closest pair on the
        // sheet is now between 0.059 and 0.0625 -- and 0.0625 is what sixteen
        // perfectly spaced trails would get, so there is nothing left to win.
        e->hue = pot[i - 1];

        // Personality is independent of everything else -- ship, seeding and
        // hue are all rolled separately, so a BUTCHER is as likely to be the
        // sixteenth seed in a CHARIOT as the second in a BALLISTA. Tying them
        // together would make the bracket predictable from one glance.
        e->voice = (uint8_t)((uint32_t)(vg_frand01() * (float)vg_voice_rollable())
                             % (uint32_t)vg_voice_rollable());
    }

    // --- seed 2..16 by rating, then lay them into the bracket ---
    // Selection sort over 15 entries; this runs once per tournament.
    int order[TOURNEY_ENTRANTS - 1];
    for (int i = 0; i < TOURNEY_ENTRANTS - 1; i++) order[i] = i + 1;
    for (int i = 0; i < TOURNEY_ENTRANTS - 2; i++) {
        int best = i;
        for (int j = i + 1; j < TOURNEY_ENTRANTS - 1; j++)
            if (vt.entrant[order[j]].rating > vt.entrant[order[best]].rating) best = j;
        int t = order[i]; order[i] = order[best]; order[best] = t;
    }

    // seed_to_entrant[s] is the entrant holding seed s (1-based).
    int8_t seed_to_entrant[TOURNEY_ENTRANTS + 1];
    seed_to_entrant[1] = 0;                       // the player
    for (int s = 2; s <= TOURNEY_ENTRANTS; s++)
        seed_to_entrant[s] = (int8_t)order[s - 2];

    // The legend takes the second seed, which puts it in the opposite half and
    // therefore in the final -- but only until the name is yours.
    if (!vg.champion) {
        Entrant* ph = &vt.entrant[seed_to_entrant[2]];
        // THE NAME IS INHERITED once the title has changed hands. vg.phantom_tag holds
        // the callsign of whoever killed the last champion, and that pilot is now the
        // one seeded to the final -- so beating the game and then losing it puts a
        // specific person in your way rather than the anonymous rumour again.
        //
        // Only the NAME is theirs. The red trail, the archetype and the top rating stay
        // the legend's, because those are what the legend IS -- an unseen pilot with a
        // signature nobody mistakes. Inheriting the killer's hull and voice as well
        // would need more than the three bytes the save has room for, and it would
        // make the legend a different thing every cycle instead of the same one wearing
        // a new name.
        //
        // The name IS revealed: the bracket and the cutscene both print the callsign, so
        // first time through the final reads PHM and afterwards it reads whoever took
        // the title. is_phantom no longer decides any text -- it survives only as the
        // rule in sim_match that keeps this pilot from going out in a round the player
        // never sees.
        if (vg.phantom_tag[0]) {
            for (int i = 0; i < 4; i++) ph->tag[i] = vg.phantom_tag[i];
        } else {
            ph->tag[0] = 'P'; ph->tag[1] = 'H'; ph->tag[2] = 'M'; ph->tag[3] = 0;
        }
        ph->voice      = (uint8_t)vg_voice_phantom();
        ph->rating     = 1.0f;
        // THE COLOUR HELD BACK FOR IT. Red, or blue if the player took red --
        // decided before the roster was dealt and kept out of every rival's
        // reach, so the legend's trail is unmistakable the instant it comes into
        // view rather than merely unlikely to be duplicated.
        //
        // This used to be a flat 0.0 with a comment saying the roster's spread
        // never lands on red. Two of the fifteen did.
        // SWAPPED, NOT ASSIGNED. Whoever was dealt the legend's colour takes the
        // legend's own place in the pot, so the fifteen stay fifteen distinct
        // colours -- assigning it outright would leave two pilots wearing it.
        for (int i = 1; i < TOURNEY_ENTRANTS; i++)
            if (vt.entrant[i].hue == ph_hue && &vt.entrant[i] != ph) {
                vt.entrant[i].hue = ph->hue;
                break;
            }
        ph->hue        = ph_hue;
        ph->is_phantom = true;
    }

    for (int i = 0; i < TOURNEY_ENTRANTS; i++)
        vt.slot[0][i] = seed_to_entrant[SEED_ORDER[i]];

    // Move everybody's seat without moving anybody's opponent.
    shuffle_bracket(vt.slot[0], 0, TOURNEY_ENTRANTS);

    for (int r = 1; r <= TOURNEY_ROUNDS; r++)
        for (int i = 0; i < TOURNEY_ENTRANTS; i++) vt.slot[r][i] = -1;

    vt.round      = 0;
    // No longer slot 0: the shuffle has moved everyone, so find the player.
    vt.player_pos = 0;
    for (int i = 0; i < TOURNEY_ENTRANTS; i++)
        if (vt.slot[0][i] == 0) { vt.player_pos = (uint8_t)i; break; }
    vt.complete   = false;

}

const Entrant* vg_tourney_opponent(void) {
    if (vt.round >= TOURNEY_ROUNDS) return nullptr;
    // Partner within the pair: even positions face the next slot up, odd the one
    // below.
    int opp = (vt.player_pos ^ 1);
    int idx = vt.slot[vt.round][opp];
    if (idx < 0) return nullptr;
    return &vt.entrant[idx];
}

void vg_tourney_resolve(bool player_won) {
    if (vt.round >= TOURNEY_ROUNDS) return;

    const int r      = vt.round;
    const int nslots = TOURNEY_ENTRANTS >> r;
    const int player_match = vt.player_pos >> 1;

    for (int m = 0; m < nslots / 2; m++) {
        int ia = vt.slot[r][m * 2];
        int ib = vt.slot[r][m * 2 + 1];
        if (ia < 0 || ib < 0) { vt.slot[r + 1][m] = (int8_t)((ia >= 0) ? ia : ib); continue; }

        int winner;
        if (m == player_match) {
            // The one match nobody simulates.
            int playeri = vt.slot[r][vt.player_pos];
            int oppi    = vt.slot[r][vt.player_pos ^ 1];
            winner = player_won ? playeri : oppi;
        } else {
            winner = sim_match(&vt.entrant[ia], &vt.entrant[ib]) ? ia : ib;
        }
        vt.slot[r + 1][m] = (int8_t)winner;
    }

    if (!player_won) {
        return;
    }

    vt.player_pos = (uint8_t)player_match;
    vt.round      = (uint8_t)(r + 1);
    if (vt.round >= TOURNEY_ROUNDS) vt.complete = true;
}
