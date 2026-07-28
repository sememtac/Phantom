#include "vg_voice.h"

// Six archetypes. Enough that a sixteen-entrant bracket does not repeat itself
// inside a single round, few enough that each one stays a recognisable person
// rather than a bag of lines -- by the semi-final you should be able to tell
// what kind of pilot you are up against from one transmission.
//
// Order within each block is TAUNT, FIRE, HURT, DEATH.

const PilotVoice vg_pilot_voice[] = {

// --- the butcher: enjoys the job, and wants you to know it -----------------
{ "BUTCHER", {
    { "FRESH MEAT ON THE SCOPE",  "I COUNT SIXTEEN CORPSES",  "STAY STILL. IT IS KINDER" },
    { "OPEN WIDE",                "THIS ONE IS FOR THE CROWD","CATCH"                    },
    { "GOOD. I FELT THAT",        "MORE. HIT ME AGAIN",       "YOU MARKED ME. CUTE"      },
    { "TELL THEM I ATE WELL",     "MY BLOOD IS BOILING OUT",  "SO THIS IS THE TASTE"     },
}},

// --- the zealot: flying is a rite, and dying is the point ------------------
{ "ZEALOT", {
    { "THE VOID IS WATCHING",     "WE ARE ALREADY ASHES",     "YOUR NAME IS WRITTEN"     },
    { "BE JUDGED",                "GO INTO THE DARK",         "DELIVERANCE"              },
    { "PAIN IS THE PRAYER",       "I AM BEING PURIFIED",      "STRIKE AGAIN, PILGRIM"    },
    { "I SEE IT. IT IS EMPTY",    "NOTHING. THERE IS NOTHING","COLD. AND NO ONE CAME"    },
}},

// --- the professional: this is a contract, and you are a line item ---------
{ "OPERATOR", {
    { "CONTRACT SAYS YOU DIE",    "SPONSORS WANT THIS QUICK", "NOTHING PERSONAL"         },
    { "ROUND AWAY",               "SOLUTION LOCKED",          "TERMINATING"              },
    { "HULL BREACH. NOTED",       "THAT WILL COST YOU",       "RECALCULATING"            },
    { "TELL MY FAMILY. NOTHING",  "THE AIR IS GONE. SO COLD", "I WAS ALMOST PAID OUT"    },
}},

// --- the hunter: the one the opening crawl warns you about -----------------
{ "HUNTER", {
    { "ANOTHER CHILD IN A SHIP",  "HOW MANY LAPS, ROOKIE",    "I HUNT THE NEW ONES"      },
    { "LEARN FROM THIS",          "TOO SLOW. TOO SLOW",       "SIT STILL, LITTLE ONE"    },
    { "OH. YOU BITE",             "LUCKY. TRY THAT AGAIN",    "NOT BAD, FOR A CHILD"     },
    { "I TAUGHT YOU NOTHING",     "MY HANDS WILL NOT MOVE",   "THE CROWD IS LAUGHING"    },
}},

// --- the broken: too many rounds, and nobody stood him down ----------------
{ "REVENANT", {
    { "DO YOU HEAR THEM TOO",     "I STOPPED COUNTING",       "THIS IS ROUND NINE OH ONE"},
    { "WAKE UP. WAKE UP",         "IT WANTS YOU NOW",         "HAHAHA"                   },
    { "THAT ONE WAS REAL",        "I FELT COLOURS",           "AGAIN. AGAIN. AGAIN"      },
    { "MOTHER. THE LIGHTS",       "I CAN SEE MY OWN SPINE",   "FINALLY. FINALLY QUIET"   },
}},

// --- the knight: the only one who thinks this is a duel --------------------
{ "PALADIN", {
    { "MAY IT BE CLEAN",          "I SALUTE YOU",             "NO TRICKS TODAY"          },
    { "GUARD YOURSELF",           "FOX TWO",                  "DEFEND"                   },
    { "WELL STRUCK",              "YOU HAVE SKILL",           "A FAIR HIT"               },
    { "IT WAS AN HONOUR",         "MY HELMET IS FULL",        "REMEMBER MY NAME"         },
}},

};

#define VOICE_COUNT ((int)(sizeof(vg_pilot_voice) / sizeof(vg_pilot_voice[0])))

int vg_voice_count(void) { return VOICE_COUNT; }

const char* vg_voice_line(uint8_t voice, VoiceEvent ev, uint32_t pick) {
    if (voice >= VOICE_COUNT) voice = 0;
    if (ev    >= VOICE_EVENTS) ev = VOICE_TAUNT;
    return vg_pilot_voice[voice].line[ev][pick % VOICE_LINES];
}

const char* vg_voice_archetype(uint8_t voice) {
    if (voice >= VOICE_COUNT) voice = 0;
    return vg_pilot_voice[voice].archetype;
}
