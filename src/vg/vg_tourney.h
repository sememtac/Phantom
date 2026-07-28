#pragma once
#include <stdint.h>
#include "vg_ship.h"

// ===========================================================================
// THE TOURNAMENT
//
// Sixteen entrants, single elimination, four rounds. The player flies four
// matches; the other eleven resolve here in simulation.
//
// The player is placed in the #1 bracket slot. That is not flattery, it is the
// difficulty curve: conventional seeding means the top slot meets #16, then the
// 8/9 winner, then the 4/5 side, then the #2 half. The opposition escalates
// every round for free, with no difficulty scalar anywhere in the code.
//
// Lose once and the run is over. There is no consolation path.
// ===========================================================================

#define TOURNEY_ENTRANTS 16
#define TOURNEY_ROUNDS   4      // R16, QF, SF, final

struct Entrant {
    char      tag[4];       // three characters and a NUL -- the callsign
    ShipClass cls;
    float     hue;          // trail colour, 0..1 -- identity, and nothing else
    uint8_t   voice;        // which pilot archetype does the talking
    float     rating;       // seeding strength, and the odds in a simulated match
    bool      is_player;
};

struct Tourney {
    Entrant entrant[TOURNEY_ENTRANTS];

    // slot[r] holds the field still standing at the START of round r, in bracket
    // order: round 0 has 16, round 1 has 8, and so on. Match m of round r is
    // slot[r][2m] against slot[r][2m+1], and the winner lands in slot[r+1][m].
    // slot[TOURNEY_ROUNDS][0] is the champion.
    int8_t  slot[TOURNEY_ROUNDS + 1][TOURNEY_ENTRANTS];

    uint8_t round;        // 0..3 -- which round is about to be, or is being, played
    uint8_t player_pos;   // the player's index within slot[round]
    bool    player_out;
    bool    complete;     // the player won the final
};

extern Tourney vt;

// Build a fresh bracket. The player takes the #1 slot flying `player_class`.
void vg_tourney_begin(ShipClass player_class);

// Who the player faces in the current round.
const Entrant* vg_tourney_opponent(void);

// Settle the round: record the player's result, simulate the other matches, and
// advance. After this, `round` has moved on unless the tournament is finished.
void vg_tourney_resolve(bool player_won);

const char* vg_tourney_round_name(uint8_t r);
