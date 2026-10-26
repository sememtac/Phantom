// The desktop entry point.
//
// On the device the Arduino core calls setup() once and loop() for ever. That is
// all this does, so main.cpp -- the whole frame sequence, the telemetry, the
// crumbs -- is the same file running here as on the board.
#include "host_window.h"
#include "host_opts.h"
#include "vg_bot.h"
#include "vg_net.h"
#include <math.h>
#include "vg_game.h"
#include "vg_ship.h"
#include "vg_tourney.h"
#include "vg_sim.h"

bool host_dataset_open(const char* path);
int g_host_shot = 0;   // --shot N: write frame N to shot.ppm, then carry on
void host_random_seed(uint32_t seed);
#include "vg_prof.h"
void host_dataset_close(void);
uint32_t host_dataset_rows(void);
const VgNetQ* host_net_load(const char* path);   // host_netfile.cpp

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vg_states.h"
#include "vg_sky.h"
#include "vg_tv.h"
#include "vg_canopy_op.h"
#include "vg_canopy_set.h"
#include "vg_cine.h"
#include "vg_pilot.h"

// Defined in src/main.cpp, compiled unchanged.
extern void setup(void);
extern void loop(void);

// ONE LINE PER BOUT, as it ends, so a run that is cut short still holds every
// bout it finished. key=value, so a reader does not depend on the column order.
// Read by tools/pilot_eval.py.
static void bout_print(const VgBout* b) {
    static const char* OUT[] = { "won", "lost", "draw" };
    static const char* BY[]  = { "missile", "ram", "wall", "other", "time" };
    const Ship* e = &vg.enemy[0];
    const float eh = (e->spec && e->spec->hull > 0.0f)
                   ? (e->alive ? e->hull / e->spec->hull : 0.0f) : 0.0f;
    printf("bout %s t=%.2f by=%s seat=%s hull0=%.3f hull=%.3f enemy=%s pilot=%s ehull=%.3f\n",
           OUT[b->outcome < 3 ? b->outcome : 2], (double)b->seconds,
           BY[b->cause < 5 ? b->cause : 3],
           vg.spec ? vg.spec->name : "-", (double)b->hull0,
           (double)((vg.health_max > 0.0f) ? vg.health / vg.health_max : 0.0f),
           e->spec ? e->spec->name : "-",
           e->pilot ? e->pilot->name : "-", (double)eh);
}

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
static int   g_prev_ship = -1;   // fly this hull first -- see the note at its use
static float g_hue     = -1.0f;  // trail colour 0..1, or leave the profile's alone

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
// A PAUSE IS TAKEN FROM A MATCH THAT IS ALREADY RUNNING, and it turns out that
// matters: the set-up alone is not a flown frame. So this puts the player in the
// seat and PWR is pressed later, from the frame loop -- see g_pause_at.
static int g_pause_at = 0;      // frames of play before the key goes down

static void screen_pause(void) {
    screen_match();
    // Long enough for the cockpit to finish booting and the canopy to finish
    // arriving. At 90 frames both are still in progress and the frame looks like
    // a fault rather than like a paused match.
    if (g_pause_at <= 0) g_pause_at = 180;
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
    { "pause",    VG_PLAYING,   screen_pause,   false, "the pause menu, over a match" },
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
    // UNBUFFERED, UNTIL THE FRAME LOOP STARTS. The game narrates its own
    // start-up over the serial port, and if that narration is sitting in a
    // buffer when something goes wrong it is lost exactly when it was about to
    // be useful. Nothing here is on a clock, so the cost does not matter.
    //
    // It matters very much once frames begin. See the setvbuf below.
    setvbuf(stdout, nullptr, _IONBF, 0);

    int scale = 2;
    int frames = 0;   // 0 = run until the window is closed
    int gym_mine = -1, gym_theirs = -1;   // <0 = do not skip the menus
    const HostScreen* screen = nullptr;    // --screen NAME: where to start
    const char* dump = nullptr;           // where to write (obs, action) pairs
    const char* net_check = nullptr;      // --net-check F: run the network on F and stop
    const char* net_file = nullptr;       // --net-file F: the opponents fly this network
    const char* seat_net_file = nullptr;  // --seat-net-file F: the player's seat flies this one
    bool headless = false;
    bool rotate = false;   // a different opponent class on every respawn
    int  pilot  = -1;      // which of the five characters the gym sends
    float bouts = 0.0f;    // --bouts S: score the gym as separate bouts; 0 = off
    float rec_target = 0.0f;   // --record-minutes M: stop the recording at M minutes
    float rec_done   = 0.0f;   // --record-done D: minutes recorded in earlier sittings
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sens") && i + 1 < argc) {
            const float v = (float)atof(argv[++i]);
            if (v > 0.0f) g_host_stick_sens = v;
        }
        // The pad's pitch the flight-stick way: pull back to climb. See host_opts.h.
        else if (!strcmp(argv[i], "--pad-invert")) g_host_pad_invert = true;
        // The softness of the middle of the pad's left stick. See host_opts.h.
        else if (!strcmp(argv[i], "--pad-expo") && i + 1 < argc)
            g_host_pad_expo = (float)atof(argv[++i]);
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
        // EVERY BOUT A FRESH ONE, for measuring. The gym on its own keeps the
        // seat's damage after a win and waits out the wreck after a loss, which is
        // right for a player and wrong for a count: a loss straight after a win is
        // a different fight. This restarts the match the moment a bout ends, both
        // ships whole, and calls a bout that runs S seconds a draw.
        else if (!strcmp(argv[i], "--bouts") && i + 1 < argc) bouts = (float)atof(argv[++i]);
        // A RECORDING THAT KNOWS WHEN IT IS DONE. The target is the TOTAL for the
        // hull across sittings, and --record-done says how much of it is already in
        // earlier files, so the clock in the corner is the hull's running total and
        // the sitting ends itself when the total is reached. The pilot console works
        // both numbers out from captures/ and passes them.
        else if (!strcmp(argv[i], "--record-minutes") && i + 1 < argc)
            rec_target = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--record-done") && i + 1 < argc)
            rec_done = (float)atof(argv[++i]);
        // The opponent is flown by the network. This is the one you fight.
        else if (!strcmp(argv[i], "--enemy-net")) vg_enemy_net = true;
        else if (!strcmp(argv[i], "--no-enemy-net")) vg_enemy_net = false;
        else if (!strcmp(argv[i], "--demo")) vg_demo_wanted = true;
        else if (!strcmp(argv[i], "--show-ai")) vg_show_ai = true;
        else if (!strcmp(argv[i], "--net-survival")) vg_net_owns_survival = true;
        else if (!strcmp(argv[i], "--no-net-survival")) vg_net_owns_survival = false;
        else if (!strcmp(argv[i], "--no-modes")) vg_bot_modes_on = false;
        // The network asked early for every class. See vg_net_all_phases.
        else if (!strcmp(argv[i], "--net-all-phases")) vg_net_all_phases = true;
        // The network flies, the class's own rules decide when to fire.
        else if (!strcmp(argv[i], "--net-rules-trigger")) vg_net_rules_trigger = true;
        // The cornered pull leaves frames the network flew alone. See vg_net_corner_exempt.
        else if (!strcmp(argv[i], "--net-corner-exempt")) vg_net_corner_exempt = true;
        // NETWORKS FROM FILES, for the training harness: see host_netfile.cpp.
        else if (!strcmp(argv[i], "--net-file") && i + 1 < argc) net_file = argv[++i];
        else if (!strcmp(argv[i], "--seat-net-file") && i + 1 < argc) seat_net_file = argv[++i];
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
        else if (!strcmp(argv[i], "--hue") && i + 1 < argc)
            g_hue = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--prev-ship") && i + 1 < argc)
            g_prev_ship = atoi(argv[++i]);
        // PWR, N frames in. Works over any screen, which is the point: what the
        // pause looks like depends entirely on what is behind it.
        else if (!strcmp(argv[i], "--pause-at") && i + 1 < argc)
            g_pause_at = atoi(argv[++i]);
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
        else if (!strcmp(argv[i], "--net-check") && i + 1 < argc) net_check = argv[++i];
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
                   "  --pad-invert Xbox pad: pull the left stick back to pitch\n"
                   "               the nose UP, like a flight stick. The default\n"
                   "               is the FPS way: push up to point the nose up.\n"
                   "  --pad-expo F Xbox pad: how soft the middle of the left\n"
                   "               stick is, 0 to 1. 0 is linear. Default 0.6. A\n"
                   "               full push always gives the full turn.\n"
                   "  --headless   no window drawn, fixed 1/60 step. Runs as fast\n"
                   "               as the machine allows.\n"
                   "  --dump F     write (observation, action) pairs to F.\n"
                   "               The recorded time shows in the top right.\n"
                   "  --record-minutes M  with --dump: stop when the total\n"
                   "               reaches M minutes. Only live flight counts:\n"
                   "               not pauses, menus or the wreck.\n"
                   "  --record-done D  minutes already recorded in earlier\n"
                   "               sittings, counted toward M. Default 0.\n"
                   "  --scripted   fly the hand-written policy, not the network.\n"
                   "  --seed N     fix the random seed. Two runs with the same seed\n"
                   "               are the same fight, which is what a measured\n"
                   "               comparison needs.\n"
                   "  --no-net-survival  hand missiles and tails back to the\n"
                   "               hand-written rules.\n"
                   "  --net-rules-trigger  with --enemy-net: the network flies,\n"
                   "               and the class's own rules decide when to fire.\n"
                   "  --net-corner-exempt  with --enemy-net: the cornered pull\n"
                   "               does not bend frames the network flew.\n"
                   "  --net-file F  the opponents fly the int8 network in F\n"
                   "               (tools/quantize_pilot.py --blob) instead of\n"
                   "               the one built into the game.\n"
                   "  --seat-net-file F  the same for the player's seat, with\n"
                   "               --bot and without --scripted.\n"
                   "  --net-check F  run the network on each row of the --dump\n"
                   "               file F, print its outputs as integers, and\n"
                   "               stop. For tools/quantize_pilot.py --check.\n"
                   "  --net-all-phases  with --enemy-net: the network is asked\n"
                   "               first for every class. Without it, CHARIOT\n"
                   "               and BALLISTA meet it only inside their tactic.\n"
                   "  --show-ai    name the layer flying the nearest opponent, on\n"
                   "               the panel. NET is the trained pilot.\n"
                   "  --demo       let the title screen play the game to itself.\n"
                   "  --no-enemy-net  hand the opponents back to the tactics.\n"
                   "  --enemy-net  the OPPONENT is flown by the network. Fly the\n"
                   "               gym as usual and you are fighting it.\n"
                   "  --pilot N    which character the gym sends: 0 RAW, 1 JRN,\n"
                   "               2 PRO, 3 VET, 4 ACE. Default 2.\n"
                   "  --bouts S    score the gym as separate bouts. Each bout\n"
                   "               starts with both ships whole; one that lasts\n"
                   "               S seconds is a draw. A 'bout' line is printed\n"
                   "               as each one ends, whether or not this is set.\n"
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
                   "  --hue F      the player's trail colour, 0..1.\n"
                   "  --pause-at N press PWR N frames in, over whatever screen.\n"
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

    // THE NETWORK AND NOTHING ELSE, on recorded observations. Each row of a --dump
    // file goes through vg_net_run, the firmware's own forward pass, and the outputs
    // are printed as the integers they were made from. tools/quantize_pilot.py
    // --check compares them with its reference, which must agree to the bit. No
    // window and no game: the network is a pure function of the observation.
    // THE LOADED NETWORKS, before anything flies -- --net-check included, so the
    // check can test a file as well as the compiled network. A file that does not
    // load stops the run: a harness that silently flew the compiled network instead
    // would score the wrong candidate.
    if (net_file) {
        const VgNetQ* q = host_net_load(net_file);
        if (!q) return 1;
        vg_net_use(0, q);
    }
    if (seat_net_file) {
        const VgNetQ* q = host_net_load(seat_net_file);
        if (!q) return 1;
        vg_net_use(1, q);
    }

    if (net_check) {
        FILE* f = fopen(net_check, "rb");
        if (!f) { fprintf(stderr, "could not open %s\n", net_check); return 1; }
        uint32_t hdr[3] = { 0, 0, 0 };
        if (fread(hdr, sizeof(uint32_t), 3, f) != 3) { fclose(f); return 1; }
        const int obs_n = (int)(hdr[1] & 0xFFFF), act_n = (int)hdr[2];
        float row[256];
        if (obs_n + act_n > 256) { fclose(f); return 1; }
        long done = 0;
        while ((!frames || done < frames) &&
               fread(row, sizeof(float), (size_t)(obs_n + act_n), f) == (size_t)(obs_n + act_n)) {
            VgNetOut o;
            o.valid = false;
            vg_net_run(row, obs_n, &o);
            if (!o.valid) { printf("invalid\n"); break; }
            printf("q %ld %ld %ld %d %d\n", lroundf(o.pitch * 127.0f), lroundf(o.yaw * 127.0f),
                   lroundf((o.throttle * 2.0f - 1.0f) * 127.0f), o.fire ? 1 : 0, o.mode);
            done++;
        }
        fclose(f);
        return 0;
    }

    vg_headless = headless;

    // OPENED BEFORE setup(), so that a window that cannot be created is reported
    // as itself rather than as the panel failing inside the game's own start-up.
    //
    // NOT OPENED WHEN HEADLESS. The call is still made, and host_window_open
    // returns at once without creating anything: a measurement run reads no keys,
    // maps no pointer and draws nothing, and a hidden window on the desktop was
    // what kept taking the focus from whoever was using the machine. See there.
    if (!host_window_open(scale, "PHANTOM")) {
        fprintf(stderr, "could not open a window\n");
        return 1;
    }

    printf("stick scale %.3f -- full deflection at %.0f mouse counts\n",
           (double)g_host_stick_sens, (double)(115.0f / g_host_stick_sens));

    setup();

    // AFTER setup(), which builds the world and lands on the title screen. Doing
    // it before would be setting up a match the boot sequence then replaces.
    //
    // THE PILOT AND THE ROTATION GO IN FIRST. vg_gym_enter spawns the first
    // opponent before it returns, so set afterwards they reached only the
    // respawns -- and every --pilot measurement in which nobody died was five
    // runs of the same PRO.
    if (gym_mine >= 0 && gym_theirs >= 0) {
        vg.gym_rotate = rotate;
        vg.gym_pilot  = (int8_t)pilot;
        vg_gym_enter((ShipClass)(gym_mine   % SHIP_CLASSES),
                     (ShipClass)(gym_theirs % SHIP_CLASSES));
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
        // BEFORE the set-up, with the ship: the roster's trail colours are dealt
        // around the player's, so a hue set afterwards would be one the rest of
        // the field had already been kept clear of a different value for.
        if (g_hue >= 0.0f) vg.trail_hue = (g_hue > 1.0f) ? 1.0f : g_hue;
        // ANOTHER HULL'S COCKPIT FIRST, which is what quitting a match and starting
        // one in a different ship does. The screen's own setup selects the real one
        // straight after, so the frame that comes out must be the same frame either
        // way -- and it was not: a cockpit carried the last hull's palette and came
        // out speckled. Anything a drawing leaves behind shows up as a pixel
        // difference between `--ship N` and `--ship N --prev-ship M`.
        if (g_prev_ship >= 0 && g_prev_ship < SHIP_CLASSES) {
            vg_canopy_op_use(vg_canopy_op_for((ShipClass)g_prev_ship));
            vg_canopy_op_tint(vg.trail_hue);
        }

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
    // BUFFERED FROM HERE, and flushed at the end of every frame.
    //
    // stdout was unbuffered for the start-up narration above, which made every
    // Serial.printf its own write. The telemetry block writes about a kilobyte
    // across EIGHT of them every two seconds, and on Windows a write costs 16 to
    // 38 ms whether it lands in a console, a pipe or a file -- the price is per
    // call, not per byte. So one frame in every two seconds took 130 to 300 ms
    // against a 16 ms budget, the audio queue ran dry, and the sound stuttered
    // on a two second clock.
    //
    // Nothing in the profile showed it. `blocked` and `short` both read zero
    // throughout, because a queue that is empty never refuses a sample and never
    // makes anybody wait. Those counters can only see the ring being too FULL.
    //
    // Buffering collapses the batch into one write. The flush keeps the crash
    // guarantee the unbuffered mode was there for: at most one frame of output
    // is lost, rather than everything since the last 64 KB.
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);

    vg_bout_tap = bout_print;
    // Counted whether or not --frames was given, and printed on the way out, so a
    // run that stopped early says so instead of passing for a short one.
    unsigned long flown = 0;

    while (host_window_pump()) {
        loop();
        flown++;
        // THE RECORDING CLOCK, and the cut-off. Rows over 60 is seconds of live
        // flight, which is exactly what the trainer will read.
        if (dump) {
            const float mins = rec_done + (float)host_dataset_rows() / 3600.0f;
            const int   secs = (int)(mins * 60.0f);
            char clock[40];
            if (rec_target > 0.0f) {
                const int ts = (int)(rec_target * 60.0f + 0.5f);
                snprintf(clock, sizeof(clock), "REC %d:%02d / %d:%02d",
                         secs / 60, secs % 60, ts / 60, ts % 60);
            } else {
                snprintf(clock, sizeof(clock), "REC %d:%02d", secs / 60, secs % 60);
            }
            host_window_set_overlay(clock);
            if (rec_target > 0.0f && mins >= rec_target) {
                printf("record: %.2f of %.2f minutes, so the sitting ends here\n",
                       (double)mins, (double)rec_target);
                break;
            }
        }
        // THE NEXT BOUT, AT ONCE AND WHOLE. See --bouts. Between frames, so the
        // frame that decided the bout has finished and printed it already. A
        // gym_arm plus a state change is exactly what the gym does itself after
        // a death, only without the wreck and with nothing carried over.
        if (bouts > 0.0f && vg.gym) {
            bool next = (vg.state == VG_KILL || vg.state == VG_OVER);
            if ((vg.state == VG_PLAYING || vg.state == VG_HIT)
                && vg_bout_time() >= bouts) {
                vg_bout_draw();
                next = true;
            }
            if (next) {
                vg_cine_clear();
                vg.gym_arm = true;
                vg_state_go(VG_PLAYING);
            }
        }
        fflush(stdout);
        // PWR, pressed from here rather than set up before the first frame. See
        // screen_pause: a pause suspends a match, and a match that has never
        // drawn a frame is not one.
        // NOT WHILE THE SET IS OFF. A cut changes state at the JOIN, so a pause
        // taken over the black is replaced by whatever the transition was on its
        // way to -- which is what happened: the sheet came up unpaused because
        // its own arrival overwrote the press. The count only runs once the
        // picture is settled, which is also when a player could press it.
        if (g_pause_at > 0 && vg_tv.phase == TV_NONE && --g_pause_at == 0) {
            vg.pause_from = (uint8_t)vg.state;
            vg.pause_t    = vg.state_t;
            vg.pause_page = 0;
            vg_state_go(VG_PAUSE);
        }
        if (frames && ++n >= frames) break;
    }

    host_dataset_close();

    // HOW FAR IT GOT. The bout still running when the run ended is printed as
    // open rather than dropped, so a long stalemate shows up as one instead of as
    // nothing at all.
    printf("frames %lu\n", flown);
    if ((vg.state == VG_PLAYING || vg.state == VG_HIT) && vg_bout_time() > 0.0f)
        printf("bout open t=%.2f\n", (double)vg_bout_time());

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
            // At the END of the line, so the readers of the fields before it do
            // not move: new passes (a lock after 2 s without one), and the share of
            // frames more than 4 s past the last lock.
            printf(" | reengage %lu dead %.0f%%", (unsigned long)g_ai_lockon[c],
                   g_ai_dead[c] * f);
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
