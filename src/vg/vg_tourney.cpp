#include "vg_tourney.h"
#include "vg_sim.h"
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
static bool sim_match(const Entrant* a, const Entrant* b) {
    float sum = a->rating + b->rating;
    float p   = 0.5f + 0.5f * ((a->rating - b->rating) / (sum > 1e-3f ? sum : 1.0f)) * 2.4f;
    if (p < 0.12f) p = 0.12f;
    if (p > 0.88f) p = 0.88f;
    return vg_frand01() < p;
}

void vg_tourney_begin(ShipClass player_class) {
    memset(&vt, 0, sizeof(vt));

    // --- the player, always seed 1 ---
    Entrant* p = &vt.entrant[0];
    // Placeholder until callsign entry exists. Three characters, like everyone
    // else's, so the bracket layout is already correct.
    strcpy(p->tag, "YOU");
    p->cls       = player_class;
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

    for (int i = 1; i < TOURNEY_ENTRANTS; i++) {
        Entrant* e = &vt.entrant[i];
        strcpy(e->tag, CALLSIGNS[pool[i - 1]]);
        e->cls = (ShipClass)((uint32_t)(vg_frand01() * SHIP_CLASSES) % SHIP_CLASSES);
        // Ship class is a sidegrade, so it must not decide the seeding -- pilot
        // quality does. Otherwise the bracket would just sort by ship and the
        // player could read the whole tournament off the class glyphs.
        e->rating    = vg_frand(0.55f, 1.00f);
        e->is_player = false;
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

    for (int i = 0; i < TOURNEY_ENTRANTS; i++)
        vt.slot[0][i] = seed_to_entrant[SEED_ORDER[i]];

    for (int r = 1; r <= TOURNEY_ROUNDS; r++)
        for (int i = 0; i < TOURNEY_ENTRANTS; i++) vt.slot[r][i] = -1;

    vt.round      = 0;
    vt.player_pos = 0;      // SEED_ORDER[0] == 1, so the player starts at slot 0
    vt.player_out = false;
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
        vt.player_out = true;
        return;
    }

    vt.player_pos = (uint8_t)player_match;
    vt.round      = (uint8_t)(r + 1);
    if (vt.round >= TOURNEY_ROUNDS) vt.complete = true;
}
