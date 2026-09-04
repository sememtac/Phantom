// The desktop entry point.
//
// On the device the Arduino core calls setup() once and loop() for ever. That is
// all this does, so main.cpp -- the whole frame sequence, the telemetry, the
// crumbs -- is the same file running here as on the board.
#include "host_window.h"
#include "host_opts.h"
#include "vg_bot.h"
#include "vg_game.h"
#include "vg_ship.h"
#include "vg_tourney.h"
#include "vg_sim.h"

bool host_dataset_open(const char* path);
int g_host_shot = 0;   // --shot N: write frame N to shot.ppm, then carry on
void host_random_seed(uint32_t seed);
#include "vg_prof.h"
void host_dataset_close(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vg_states.h"
#include "vg_sky.h"

// Defined in src/main.cpp, compiled unchanged.
extern void setup(void);
extern void loop(void);

// ===========================================================================
// WHERE TO START, AND WHAT HAS TO BE TRUE FOR IT TO MEAN ANYTHING
//
// Every page in the game, reachable in one command, with enough state behind it
// to be worth looking at. It was five ad-hoc flags -- entry, select, pause,
// course, bracket -- each with its own bool, its own arm of the parser and its
// own line in the apply block, and half the screens had no way in at all.
//
// The cost of that is not tidiness. A page you cannot reach without clicking
// through three menus is a page nobody looks at after a change, and the repair
// screen and the round-won card went unlooked-at for exactly that reason.
//
// A ROW PER SCREEN, the same shape as STATES[] in vg_states.cpp and for the same
// reason: adding a page should be adding a line.
// ===========================================================================

static int   g_rounds  = 0;      // rounds already settled
static int   g_credits = -1;     // the bank, or leave the profile's alone
static float g_hull    = -1.0f;  // fraction of full, or leave it whole
static int   g_ship    = -1;     // class index, or leave the profile's alone

// A TOURNAMENT TO LOOK AT: the draw, and however many rounds are behind it. The
// player wins every one, because a loss ends the run and there is no sheet to
// look at afterwards.
static void screen_tourney(void) {
    vg_tournament_begin(vg.ship);
    for (int r = 0; r < g_rounds && r < TOURNEY_ROUNDS; r++)
        vg_tourney_resolve(true);
}

static void screen_none(void) {}

// BOTH HALVES OF A MATCH, and the backdrop the cutscene would have raised.
//
// This is vg_gym_start's problem with no gym in it: a tournament runs
// vg_match_start at one end of the launch cutscene and vg_begin_flight at the
// other, so calling only the first comes up with no cockpit drawing selected, the
// boot chain disarmed and a dark panel. See the note there, which was written
// after exactly that.
static void screen_match(void) {
    screen_tourney();
    vg_match_start();
    vg_begin_flight();
    vg_sky_set_reveal(1.0f);
}

// A PAUSE IS NOT A PLACE, it suspends one -- so it needs something to suspend.
// The flag used to cut straight to VG_PAUSE over whatever the boot sequence had
// left on the screen, which is a pause menu floating over the title.
//
// A match, because that is where a player meets it. Pausing over a PAGE is the
// other half of the same key and is reached by pressing PWR on one.
static void screen_pause(void) {
    screen_match();
    vg.pause_from = (uint8_t)VG_PLAYING;
    vg.pause_t    = 0.0f;
    vg.pause_page = 0;
}

struct HostScreen {
    const char* name;
    VgState     state;
    void      (*setup)(void);   // what has to exist before the page means anything
    // HOW TO ARRIVE. A CUT is the broadcast going off and coming back -- the set
    // collapses to a scan band and the state changes at the join, which is right
    // for moving between PLACES. A pause is not a place: it suspends one, and
    // cutting into it tore down the match it was meant to be suspending, so the
    // menu came up over a black screen.
    bool        cut;
    const char* what;
};

static const HostScreen SCREENS[] = {
    { "title",    VG_ATTRACT,   screen_none,    true,  "the attract screen" },
    { "entry",    VG_ENTRY,     screen_none,    true,  "callsign registration" },
    { "select",   VG_SELECT,    screen_none,    true,  "ship select" },
    { "bracket",  VG_BRACKET,   screen_tourney, true,  "the tournament sheet" },
    { "repair",   VG_REPAIR,    screen_tourney, true,  "the repair page" },
    { "course",   VG_COURSE,    screen_none,    true,  "the ring course" },
    { "pause",    VG_PAUSE,     screen_pause,   false, "the pause menu, over a match" },
    { "intro",    VG_INTRO,     screen_tourney, true,  "the launch cutscene" },
    { "match",    VG_PLAYING,   screen_match,   true,  "flying, against the bracket" },
    { "roundwon", VG_ROUND_WON, screen_tourney, true,  "the round-won card" },
    { "over",     VG_OVER,      screen_tourney, true,  "knocked out" },
    { "won",      VG_WON,       screen_tourney, true,  "took the whole thing" },
};

#define SCREEN_N ((int)(sizeof(SCREENS) / sizeof(SCREENS[0])))

static const HostScreen* screen_by_name(const char* n) {
    for (int i = 0; i < SCREEN_N; i++)
        if (!strcmp(SCREENS[i].name, n)) return &SCREENS[i];
    return nullptr;
}

int main(int argc, char** argv) {
    // UNBUFFERED. The game narrates its own start-up over the serial port, and
    // if that narration is sitting in a buffer when something goes wrong it is
    // lost exactly when it was about to be useful.
    setvbuf(stdout, nullptr, _IONBF, 0);

    int scale = 2;
    int frames = 0;   // 0 = run until the window is closed
    int gym_mine = -1, gym_theirs = -1;   // <0 = do not skip the menus
    const HostScreen* screen = nullptr;    // --screen NAME: where to start
    const char* dump = nullptr;           // where to write (obs, action) pairs
    bool headless = false;
    bool rotate = false;   // a different opponent class on every respawn
    int  pilot  = -1;      // which of the five characters the gym sends
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sens") && i + 1 < argc) {
            const float v = (float)atof(argv[++i]);
            if (v > 0.0f) g_host_stick_sens = v;
        }
        // THE SEAT, FLOWN BY THE GAME. Enter the gym as usual and watch the two
        // of them fight -- which is the point: a matchup nobody is holding can be
        // run for as long as it takes to mean something.
        else if (!strcmp(argv[i], "--bot")) vg_bot_on = true;
        else if (!strcmp(argv[i], "--headless")) headless = true;
        // WHAT THE SHIP GATE SEES, printed and nothing else. The inspector reads
        // this rather than parsing the class table, so the numbers it shows are
        // the numbers the gate actually compares.
        else if (!strcmp(argv[i], "--airframes")) {
            for (int c = 0; c < SHIP_CLASSES; c++) {
                const ShipSpec* sp = &vg_ship_class[c];
                float f[11];
                vg_bot_airframe(sp, f);
                printf("airframe %s", sp->name);
                for (int k = 0; k < 11; k++) printf(" %.6f", f[k]);
                printf("\n");
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--scripted")) vg_bot_net = false;
        else if (!strcmp(argv[i], "--rotate")) rotate = true;
        else if (!strcmp(argv[i], "--pilot") && i + 1 < argc) pilot = atoi(argv[++i]);
        // The opponent is flown by the network. This is the one you fight.
        else if (!strcmp(argv[i], "--enemy-net")) vg_enemy_net = true;
        else if (!strcmp(argv[i], "--no-enemy-net")) vg_enemy_net = false;
        else if (!strcmp(argv[i], "--demo")) vg_demo_wanted = true;
        else if (!strcmp(argv[i], "--show-ai")) vg_show_ai = true;
        else if (!strcmp(argv[i], "--net-survival")) vg_net_owns_survival = true;
        else if (!strcmp(argv[i], "--no-net-survival")) vg_net_owns_survival = false;
        else if (!strcmp(argv[i], "--no-modes")) vg_bot_modes_on = false;
        else if (!strcmp(argv[i], "--park")) vg_bot_park = true;
        else if (!strcmp(argv[i], "--no-ram")) vg_no_ram = true;
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) {
            screen = screen_by_name(argv[++i]);
            if (!screen) {
                fprintf(stderr, "no screen called %s. one of:", argv[i]);
                for (int k = 0; k < SCREEN_N; k++)
                    fprintf(stderr, " %s", SCREENS[k].name);
                fprintf(stderr, "\n");
                return 1;
            }
        }
        // --- the state a page is looked at against ---
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
            g_rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--credits") && i + 1 < argc)
            g_credits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hull") && i + 1 < argc)
            g_hull = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--ship") && i + 1 < argc)
            g_ship = atoi(argv[++i]);
        // THE OLD NAMES STILL WORK, and they are aliases rather than a second
        // mechanism. Scripts, baselines and half the notes name them.
        //
        // The trailing number is OPTIONAL on both, so "--select" alone behaves as
        // it always did. Sniffed for a digit rather than consumed blindly, or
        // "--select --headless" would eat the next flag.
        else if (!strcmp(argv[i], "--course")) screen = screen_by_name("course");
        else if (!strcmp(argv[i], "--entry"))  screen = screen_by_name("entry");
        else if (!strcmp(argv[i], "--pause"))  screen = screen_by_name("pause");
        else if (!strcmp(argv[i], "--bracket")) {
            screen = screen_by_name("bracket");
            if (i + 1 < argc && argv[i + 1][0] >= (int)('0') && argv[i + 1][0] <= (int)('9'))
                g_rounds = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--select")) {
            screen = screen_by_name("select");
            if (i + 1 < argc && argv[i + 1][0] >= (int)('0') && argv[i + 1][0] <= (int)('9'))
                g_ship = atoi(argv[++i]);
        }
        // A SHOT PINS THE CLOCK. Frame N has to be the same frame in two runs or
        // the file cannot be compared with anything -- see vg_fixed_dt.
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            g_host_shot = atoi(argv[++i]);
            vg_fixed_dt = true;
        }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            host_random_seed((uint32_t)strtoul(argv[++i], nullptr, 10));
        else if (!strcmp(argv[i], "--agg") && i + 1 < argc)
            vg_agg_bias = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        // A matchup, by class index, entered without touching the menus.
        else if (!strcmp(argv[i], "--gym") && i + 2 < argc) {
            gym_mine   = atoi(argv[++i]);
            gym_theirs = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--help")) {
            printf("phantom [--scale N] [--sens F] [--frames N]\n"
                   "  --scale N    window size, N times the 480x480 panel (1-4, default 2)\n"
                   "  --sens F     stick scale, logical pixels per raw mouse\n"
                   "               count. Default 0.10. Lower is slower. Full\n"
                   "               deflection costs 115/F counts of hand\n"
                   "               movement, about 3 cm at 1000 DPI by default.\n"
                   "               Raw counts, so it is never accelerated.\n"
                   "  --headless   no window drawn, fixed 1/60 step. Runs as fast\n"
                   "               as the machine allows.\n"
                   "  --dump F     write (observation, action) pairs to F.\n"
                   "  --scripted   fly the hand-written policy, not the network.\n"
                   "  --seed N     fix the random seed. Two runs with the same seed\n"
                   "               are the same fight, which is what a measured\n"
                   "               comparison needs.\n"
                   "  --no-net-survival  hand missiles and tails back to the\n"
                   "               hand-written rules.\n"
                   "  --show-ai    name the layer flying the nearest opponent, on\n"
                   "               the panel. NET is the trained pilot.\n"
                   "  --demo       let the title screen play the game to itself.\n"
                   "  --no-enemy-net  hand the opponents back to the tactics.\n"
                   "  --enemy-net  the OPPONENT is flown by the network. Fly the\n"
                   "               gym as usual and you are fighting it.\n"
                   "  --pilot N    which character the gym sends: 0 RAW, 1 JRN,\n"
                   "               2 PRO, 3 VET, 4 ACE. Default 2.\n"
                   "  --rotate     send a different opponent class each respawn.\n"
                   "               Use it when you record: one sitting then\n"
                   "               covers all four instead of one.\n"
                   "  --gym A B    start a gym match at once: class A against\n"
                   "               class B. 0 AEGIS 1 LANCE 2 CHARIOT 3 BALLISTA.\n"
                   "  --bot        the game flies the player's seat too. With\n"
                   "               --gym that is a fight nobody is holding.\n"
                   "  --screen S   start on a page, with state behind it: title\n"
                   "               entry select bracket repair course pause intro\n"
                   "               match roundwon over won.\n"
                   "  --rounds N   settle N rounds first, so a page has a past.\n"
                   "  --credits N  set the bank. REPAIR lights or dims on it.\n"
                   "  --hull F     set the hull to F of full, 0..1.\n"
                   "  --ship N     fly class N.\n"
                   "  --course     start on the practice range, past the menus.\n"
                   "  --entry      start on callsign registration.\n"
                   "  --pause      start on the pause screen.\n"
                   "  --select [N] start on the ship-select screen, on class N.\n"
                   "  --bracket [N] start on the tournament sheet, with N rounds\n"
                   "               already settled.\n"
                   "  --shot N     write frame N to shot.ppm and keep running. The\n"
                   "               frame clock is pinned to 1/60 so that frame N is\n"
                   "               the same frame in every run.\n"
                   "  --no-ram     no opponent rolls a suicide run. For testing:\n"
                   "               a rammer ends the engagement being measured.\n"
                   "  --frames N   run N frames and exit -- a smoke test, not a mode\n");
            return 0;
        }
    }

    vg_headless = headless;

    // OPENED BEFORE setup(), so that a window that cannot be created is reported
    // as itself rather than as the panel failing inside the game's own start-up.
    //
    // STILL OPENED WHEN HEADLESS, which is worth saying so it does not read as an
    // oversight: the game reads the keyboard and the pointer through the window.
    // Nothing is drawn into it -- the frame returns before the renderer -- so it
    // costs a message pump and shows whatever was last on it.
    if (!host_window_open(scale, "PHANTOM")) {
        fprintf(stderr, "could not open a window\n");
        return 1;
    }

    printf("stick scale %.3f -- full deflection at %.0f mouse counts\n",
           (double)g_host_stick_sens, (double)(115.0f / g_host_stick_sens));

    setup();

    // AFTER setup(), which builds the world and lands on the title screen. Doing
    // it before would be setting up a match the boot sequence then replaces.
    if (gym_mine >= 0 && gym_theirs >= 0) {
        vg_gym_enter((ShipClass)(gym_mine   % SHIP_CLASSES),
                     (ShipClass)(gym_theirs % SHIP_CLASSES));
        vg.gym_rotate = rotate;
        vg.gym_pilot  = (int8_t)pilot;
    }

    // STRAIGHT TO A PAGE. After setup() for the same reason the gym is: the boot
    // sequence lands on the title screen and would replace anything set up
    // before it.
    //
    // THE ORDER IS THE POINT. The ship first, because the tournament is built
    // around it and vg_match_start reads health_max off its spec. The screen's
    // own set-up next. Then the bank and the hull LAST, because building a
    // tournament heals the player and starting a match re-reads the maximum --
    // either would undo a hull set before them.
    if (screen) {
        if (g_ship >= 0 && g_ship < SHIP_CLASSES)
            vg_game_select_ship((ShipClass)g_ship);

        if (screen->setup) screen->setup();

        if (g_credits >= 0) vg.credits = g_credits;
        if (g_hull >= 0.0f) {
            float f = g_hull;
            if (f > 1.0f) f = 1.0f;
            // Never to nothing. A hull at zero is a death on the next frame,
            // which is not a screen anybody asked to look at.
            if (f < 0.02f) f = 0.02f;
            vg.health = vg.health_max * f;
        }

        // A cut for a place, a plain change for a pause. See HostScreen::cut.
        if (screen->cut) vg_state_cut(screen->state);
        else             vg_state_go(screen->state);
    }

    if (dump && !host_dataset_open(dump)) {
        fprintf(stderr, "could not open %s for writing\n", dump);
        return 1;
    }

    int n = 0;
    while (host_window_pump()) {
        loop();
        if (frames && ++n >= frames) break;
    }

    host_dataset_close();

    // WHAT THE PILOTS DID, for the same reason the missile counters print here:
    // a headless run returns from the frame before the telemetry block, so
    // nothing it counted is ever shown otherwise.
    {
        static const char* K[] = { "-","WALL","RAM","EVADE","TAIL","DRY","PRESS",
                                   "NET","TACTIC","RESET","CORNER","SUPPORT" };
        for (int c = 0; c < SHIP_CLASSES; c++) {
            if (!g_ai_frames[c]) continue;
            const double f = 100.0 / (double)g_ai_frames[c];
            printf("ai %-8s frames %lu | aim %.3f | locked %.0f%% | armed %.1f%%"
                   " | mean range %.0f |",
                   vg_ship_class[c].name, (unsigned long)g_ai_frames[c],
                   g_ai_aim[c] / (float)g_ai_frames[c], g_ai_locked[c] * f,
                   g_ai_armed[c] * f, g_ai_range[c] / (float)g_ai_frames[c]);
            for (int k = 0; k < STEER_KINDS; k++)
                if (g_ai_steer[c * STEER_KINDS + k])
                    printf(" %s=%.0f%%/%.2f", K[k], g_ai_steer[c * STEER_KINDS + k] * f,
                           g_ai_aimk[c * STEER_KINDS + k]
                           / (float)g_ai_steer[c * STEER_KINDS + k]);
            printf("\n");
        }
    }

    // WHERE THE ROUNDS WENT. A headless run returns from the frame before the
    // telemetry block, so the counters a played session prints every two seconds
    // are never shown. They are the point of a measured run, so they are printed
    // once on the way out.
    // BOTH SIDES. The counters are indexed [0] the enemy fired it, [1] the player
    // did -- and for as long as this existed only [0] was ever printed. The
    // player's half was counted every frame and thrown away at the door.
    //
    // Not a cosmetic omission. It made a HUMAN's flying the one thing the
    // instruments could not see, which is exactly the comparison worth having:
    // the scripted seat is a weak pilot, and every class number ever read off
    // this was the weak pilot's. A player flew a CHARIOT for five minutes to
    // settle whether it was too lethal, and the run reported only what the AEGIS
    // had done to them.
    for (int side = 0; side < 2; side++) {
        if (g_msl_end[side] == 0) continue;
        printf("mslend %-4s = hit %lu near %lu fuse %lu wall %lu gone %lu"
               " | lock %lu | lost illum %lu (dead %lu) cone %lu | dark %lu relit %lu | dmg %lu\n",
               side ? "you" : "them",
               (unsigned long)g_msl_why[side][0], (unsigned long)g_msl_why[side][1],
               (unsigned long)g_msl_why[side][2], (unsigned long)g_msl_why[side][3],
               (unsigned long)g_msl_why[side][4], (unsigned long)g_msl_endlock[side],
               (unsigned long)g_msl_lost[side][0], (unsigned long)g_msl_lost_dead[side],
               (unsigned long)g_msl_lost[side][1],
               (unsigned long)g_msl_dark[side], (unsigned long)g_msl_relit[side],
               (unsigned long)g_msl_dmg[side]);
    }
    return 0;
}
