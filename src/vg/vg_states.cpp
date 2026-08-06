#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_screens.h"
#include "vg_save.h"
#include "vg_cine.h"
#include "vg_course.h"
#include "vg_sfx.h"
#include "vg_shake.h"
#include "vg_replay.h"
#include "vg_raster.h"
#include "vg_canopy_set.h"
#include <math.h>

// The state machine: the table, what arriving at each state sets up, the three
// ways of getting to one, and the set turning on and off in between.
//
// Read the note above VgStateDef before changing anything here. The reason the
// table exists at all is that a state's properties used to be spread over five
// hand-kept lists in four files.

// Out of combat the backdrop is the menu's own: one fixed nebula, the same one
// every time, so the title screen is a place rather than a random wash. See
// vg_sky_menu -- it is free unless a venue has displaced it, so moving between
// the title, the bracket and the repair screen costs nothing.
void vg_use_menu_sky(void) {
    vg_sky_menu();
}

// STATIC, so that this cannot be called as though it were a transition.
//
// It was public, and the one caller outside this file used it as one: an enter hook
// sets a state up, it does not go there, so calling it left the game in whatever
// state it was already in with the attract scene built underneath. That was reachable
// only by winning the tournament, which is why it survived.
//
// Private, the table is the only thing that can reach it and the compiler enforces
// what a comment could only ask for. Everything else uses vg_state_cut or
// vg_state_go, which change the state and then let the table run this.
static void enter_attract(void) {
    vg_use_menu_sky();
    for (int i = 0; i < MAX_ENEMIES;  i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive   = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;

    vg.trail_n     = 0;
    vg.trail_head  = 0;
    vg.trail_acc   = 0;
    vg.msl_event   = MSL_NONE;
    vg.msl_event_t = 0;
    vg.threat.on      = false;
    vg.wpn.target = -1;
    vg.wpn.locked      = false;
    vg.hit_flash   = 0;
    vg_shake_clear();
}

// Every menu state flies the same idle scene underneath, so the tournament map
// and the ship select sit over moving space rather than a dead background.
void vg_menu_world(float dt) {
    // Nobody is holding the roll key on a menu, and the airframe buzz reads
    // vg.roll_rate whatever the state. Left alone, a player who backed out of the
    // course mid-roll would carry that rattle into the tournament map and keep it
    // there. The menu's own slow tumble below is a local, and unrelated.
    vg.roll_rate = 0.0f;

    float pitch_in, yaw_in;
    vg_attract_autopilot(vg.state_t, &pitch_in, &yaw_in);

    // Continuous roll rather than an oscillation. Something adrift does not rock
    // back to level -- it keeps going, slowly, and that is also what makes the
    // third axis unmistakable. About two minutes per revolution, breathing a
    // little so it never reads as a motor.
    const float roll_rate = 0.052f + 0.020f * sinf(vg.state_t * 0.023f);

    vg_world_step(dt, pitch_in, yaw_in, roll_rate * dt, 0.30f);

    vg.spawn_t -= dt;
    if (vg.spawn_t <= 0) { vg_spawn_asteroid(); vg.spawn_t = vg_frand(0.8f, 1.6f); }
    vg_update_missiles(dt);
}

// ---------------------------------------------------------------------------
// The set turning on and off
// ---------------------------------------------------------------------------

static void enter_course(void) {
    vg_arena_init(ARENA_TORUS);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
    for (int i = 0; i < MAX_ENEMIES;  i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive  = false;
    for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive  = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;
    // The course must ask for its own sky: coming from the menu there is no
    // backdrop at all, and the course is a place in the tournament's universe
    // rather than a void. Same three backdrops a match uses, drawn by the same
    // rule -- one rand, so the replay sequence keeps its length.
    const uint32_t sky_seed = vg_replay_rand();
    vg_sky_generate((SkyKind)(sky_seed % (uint32_t)SKY_KINDS), sky_seed);
    // Trim the field to the number a match flies with. The menu spawner has no
    // cap -- it tops up every second or so until all sixteen slots are full,
    // because nothing in a menu cares -- and the course inherited that field
    // wholesale. Sixteen close rocks was never a design decision; it was the
    // attract loop's housekeeping leaking into the one place that pays frame
    // time for it. The farthest go first, so what the player sees stays.
    {
        int alive = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++) if (vg.ast[i].alive) alive++;
        while (alive > AST_TARGET_COUNT) {
            int   far = -1;
            float d2  = -1.0f;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                if (!vg.ast[i].alive) continue;
                const float d = vlen2(vg.ast[i].pos);
                if (d > d2) { d2 = d; far = i; }
            }
            vg.ast[far].alive = false;
            alive--;
        }
    }
    vg_course_begin();
    vg.roll      = 0;
    vg.roll_rate = 0;
    vg.bank      = 0;
    // THE COCKPIT FIRST, and everything else in a chain behind it. hud_boot is NOT set here any
    // more, nor is SFX_READY played: the view sits dark, the cockpit arrives a region at a time,
    // and vg_hud_decay cues the instruments off its progress. Starting all three at once is what
    // made them overlap too tightly to read.
    vg.hud_cued    = false;
    vg.ready       = false;
    vg.regions_lit = 0;
    // WHICH COCKPIT, before the sequence that lights it. vg_canopy_use drops the colour table
    // and the warp maps so they rebuild against the new drawing, and vg_canopy_intro_begin sizes
    // itself from the drawing's region count -- so the order here is not cosmetic.
    vg_canopy_use(vg_canopy_for(vg.ship));
    vg_canopy_intro_begin();
    // THE ONLY SOUND THAT PLAYS TO A BLACK SCREEN. It fills the second of dark before the first
    // region lights, so the wait reads as something spooling up rather than as nothing happening --
    // which is the difference between a pause and a hang. Fired here and not from the sequence
    // itself: the sequence lives in the band rasteriser, and a cue triggered from drawing code is
    // a cue that does not happen when the panel is not drawn.
    vg_sfx_play(SFX_SPOOL, 1.0f);
    vg_input_calibrate();
}

// Arriving at the tournament table. Pulled out of the transition's switch so
// that it is the STATE's set-up and not one caller's: the table is reachable
// from the transition, from the repair screen and from the end of a round, and
// each of those used to prepare it differently.
static void enter_bracket(void) {
    // The table is a menu, so it gets the menu's backdrop -- it used to keep
    // whatever the last venue built.
    vg_use_menu_sky();
    vg.course.ring_alive = false;
    vg_bracket_focus_player();
}

// Arriving at the cutscene, which is where a match is actually built.
//
// This exists so the table does not point at vg_match_start directly. Every other
// row names an enter_* or a null, and the one cell that broke the convention was
// the cell doing the most work in the game -- the bracket draw, the venue, the
// sky, the opponent, the whole match. A reader scanning the column for what
// happens on entry saw twelve familiar names and one that looked like it had been
// wired up from somewhere else.
static void enter_intro(void) {
    vg_match_start();
}

// Leaving the cutscene. Its own state, and nothing about the match: the shot is
// finished with and the cockpit is never zoomed.
static void leave_intro(void) {
    vg_cine_clear();
    vg.cam_zoom = 1.0f;
}

// Starting the flight phase of a match.
//
// NOT an entry hook, and the note on VgStateDef::leave says why: VG_HIT hands
// control back to VG_PLAYING through vg_state_go, so anything hung on PLAYING's
// entry would run again after every hit the player takes.
//
// The cutscene has spent seventeen seconds turning and drifting the viewpoint for
// the sake of framing, and relocating between setups moved it again -- so where
// it ended up relative to the arena is not something a match should have to
// inherit. Both the arena and the opponent are re-established from scratch, which
// makes a bad start structurally impossible rather than merely unlikely. Free to
// do because the handover is a hard cut to black with the instruments rebooting
// over it: none of the snap is visible.
void vg_begin_flight(void) {
    vg_arena_init(ARENA_TORUS);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive  = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive  = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;
    vg_spawn_opponent();

    vg.hud_cued    = false;         // the boot chain again -- see enter_course
    vg.ready       = false;
    vg.regions_lit = 0;
    // WHICH COCKPIT, before the sequence that lights it. vg_canopy_use drops the colour table
    // and the warp maps so they rebuild against the new drawing, and vg_canopy_intro_begin sizes
    // itself from the drawing's region count -- so the order here is not cosmetic.
    vg_canopy_use(vg_canopy_for(vg.ship));
    vg_canopy_intro_begin();
    vg_sfx_play(SFX_SPOOL, 1.0f);
    vg.roll     = 0;
    vg.bank     = 0;
    vg.taunt_t  = BOOT_FIRST_TAUNT; // one number for the beat, not 1.6 here and 1.4 there
    vg_input_calibrate();
}

// ---------------------------------------------------------------------------
// THE STATES
//
// One row each: what a state is called, what it is, and what arriving at it
// sets up.
//
// There is no column for "arriving here cuts through the set". There was, and
// it was wrong: the tournament table is arrived at instantly from the repair
// screen and from the end of a round, and on a cut from the course and from the
// pause menu. A cut belongs to the edge, and the caller says so.
//
// The point of the table is not tidiness. It is that a state's properties are
// declared ONCE. They used to be spread over five hand-kept lists in four
// files, and the failure mode of a hand-kept list is not that it is wrong when
// written -- it is that adding a state silently leaves it right in four places
// and wrong in the fifth, with nothing to point at the one that was missed.
//
// `enter` is the set-up that arriving at the state requires, and it belongs to
// the STATE rather than to whoever sent us there. That distinction is not
// theoretical: the course reached VG_COURSE without ever generating a sky,
// because entering a state had no fixed meaning and each caller did whatever it
// remembered to. The backdrop was then whatever the last scene had built.
//
// The names are duplicated in vg_crumb.cpp, deliberately -- see the note there.
// VG_STATE_COUNT is what keeps the two the same length.
struct VgStateDef {
    const char* name;
    uint8_t     flags;
    void      (*enter)(void); // may be null: not every state sets anything up
    // ...and what leaving it has to undo. Runs BEFORE the state changes, on
    // every way out, which is the point: the INTRO teardown used to be written
    // out at the one exit that happened to exist, and it kept mutating state
    // AFTER the entry hook for the next state had already run.
    //
    // NOTE THE ASYMMETRY, and do not "fix" it. An entry hook cannot hold a
    // state's set-up unless every entry wants it, and VG_PLAYING is entered
    // twice: once at the start of a match and once every time VG_HIT hands
    // control back. An enter_playing that rebuilt the arena, emptied the racks
    // and respawned the opponent would do all of that after each hit taken. So
    // the match set-up is vg_begin_flight, called by name, and PLAYING's enter
    // column stays null on purpose.
    void      (*leave)(void);
    // One frame of being in this state. Thirteen functions in vg_game.cpp, one
    // per row, replacing a 390-line switch on exactly this value.
    void      (*update)(float dt, const VgInput* in, const Tap* tap);
};

// In enum order. Positional, like the crumb table, so the two read the same way
// side by side.
static const VgStateDef STATES[VG_STATE_COUNT] = {
    { "ATTRACT",   VGS_MENU | VGS_DRIFT,               enter_attract,    nullptr, vg_upd_attract },
    { "ENTRY",     VGS_MENU | VGS_DRIFT,               nullptr,          nullptr, vg_upd_entry },
    { "SELECT",    VGS_MENU | VGS_DRIFT,               nullptr,          nullptr, vg_upd_select },
    { "REPAIR",    VGS_MENU | VGS_DRIFT,               nullptr,          nullptr, vg_upd_repair },
    { "BRACKET",   VGS_MENU | VGS_DRIFT,               enter_bracket,    nullptr, vg_upd_bracket },
    { "INTRO",     VGS_MENU,                           enter_intro,      leave_intro, vg_upd_intro },
    { "PLAYING",   VGS_LIVE | VGS_ENGINE | VGS_COMBAT, nullptr,          nullptr, vg_upd_playing },
    { "HIT",       VGS_LIVE | VGS_ENGINE | VGS_COMBAT, nullptr,          nullptr, vg_upd_playing },
    // Still flying, and that is the whole of it: the opponent is down and
    // talking, the player cannot be hurt, and cutting the hum at that moment
    // would be the loudest thing about it.
    { "KILL",      VGS_ENGINE,                         nullptr,          nullptr, vg_upd_kill },
    // Nothing. A pause is not a place -- it suspends one.
    { "PAUSE",     0,                                  nullptr,          nullptr, vg_upd_pause },
    { "COURSE",    VGS_LIVE | VGS_ENGINE,              enter_course,     nullptr, vg_upd_course },
    { "ROUND_WON", VGS_MENU | VGS_DRIFT,               nullptr,          nullptr, vg_upd_round_won },
    { "OVER",      VGS_MENU,                           nullptr,          nullptr, vg_upd_over },
    { "WON",       VGS_MENU | VGS_DRIFT,               nullptr,          nullptr, vg_upd_won },
};

static_assert(sizeof(STATES) / sizeof(STATES[0]) == VG_STATE_COUNT,
              "a state was added without a row, or a row without a state");
static_assert((int)VG_WON + 1 == VG_STATE_COUNT,
              "VG_STATE_COUNT does not match the enum -- vg_crumb.cpp's copy "
              "of the names is the same length and would now be misaligned");

uint8_t vg_state_flags(VgState s) {
    return ((int)s < VG_STATE_COUNT) ? STATES[s].flags : 0u;
}

void vg_state_go(VgState to) {
    if ((int)to >= VG_STATE_COUNT) return;
    const VgStateDef* d = &STATES[to];

    // The clock belongs to the state, not to the caller. It was reset by hand at
    // all nineteen sites that changed state, and it happened to be right at all
    // nineteen -- which is the good version of a rule that nothing enforces.
    // Out before in, and the old state is still current while its leave runs.
    // tv_join routes through here too, so a cut gets the same teardown as a
    // direct change without a second path to keep in step.
    if ((int)vg.state < VG_STATE_COUNT) {
        const VgStateDef* from = &STATES[vg.state];
        if (from->leave) from->leave();
    }

    vg.state   = to;
    vg.state_t = 0.0f;
    if (d->enter) d->enter();
}

void vg_state_resume(VgState to) {
    if ((int)to >= VG_STATE_COUNT) return;
    vg.state   = to;
    vg.state_t = 0.0f;
}

void vg_state_cut(VgState to) {
    if (vg.tv.phase != TV_NONE) return;   // one transition at a time
    // THE SHIP IS OFF BEFORE THE PICTURE IS. Gating the per-frame sources below
    // stops them being asked for again, but it cannot retract what is already
    // sounding -- an alert beep, a hull hit, the tail of a transmission -- and
    // those carried on over the black. The set going off is the end of the
    // session, so it takes everything with it and then makes its own noise.
    vg_sfx_silence();
    // AND THE BROADCAST IS OFF THE AIR. Same rule as the audio and for the same
    // reason: a cut ends the session, and an announcement is part of one. Skip
    // out of the course while the announcer is mid-line and the line used to
    // ride the transition and finish over the tournament table -- a system
    // message about a course the player has just left.
    //
    // The CUT, not every change of state. IFT_MATCH_END is posted over the
    // wreck and is meant to run on through the redraw, and that path is a
    // vg_state_go rather than a cut. The set never goes off, so the announcer
    // never does either.
    vg_ift_clear();
    vg_sfx_play(SFX_TV_OFF, 1.0f);
    vg.tv.phase = TV_OUT;
    vg.tv.to    = (uint8_t)to;
    vg.tv.t     = 0.0f;
}

// Runs at the JOIN, with the screen black. The set-up a state needs happens here
// rather than at the button, so the old scene is never the one the aperture
// opens back onto.
//
// It used to be a switch with a case for each destination, each calling that
// destination's set-up. There is nothing left to switch on: the far side of a
// cut is a state, and entering a state is one thing.
// The dispatch. Here rather than in vg_game.cpp because the table is here, and
// because "what does this state do this frame" is a table lookup now.
void vg_state_update(float dt, const VgInput* in, const Tap* tap) {
    if ((int)vg.state >= VG_STATE_COUNT) return;
    const VgStateDef* d = &STATES[vg.state];
    if (d->update) d->update(dt, in, tap);
}

static void tv_join(void) {
    vg_state_go((VgState)vg.tv.to);
}

void vg_tv_update(float dt) {
    if (vg.tv.phase == TV_NONE) return;
    vg.tv.t += dt;

    if (vg.tv.phase == TV_OUT) {
        if (vg.tv.t >= TV_OUT_TIME) { vg.tv.phase = TV_HOLD; vg.tv.t = 0.0f; }
        return;
    }
    if (vg.tv.phase == TV_HOLD) {
        // THE SWITCH HAPPENS AT THE END OF THE DEAD AIR, not the start of it.
        //
        // Joining at the start would let the new scene run for a whole second
        // behind a black screen -- and the first thing a scene does is start
        // talking. The opening line of the ring course would have been a third
        // spent before the picture existed to show it, which is a mistake already
        // made once in this file's history and not worth making twice.
        if (vg.tv.t >= TV_HOLD_TIME) {
            tv_join();
            vg.tv.phase = TV_IN;
            vg.tv.t     = 0.0f;
            // With the picture, not before it: the thump and the dot are the
            // same moment, and hearing it during the dead air would put the
            // sound of the set striking over a screen that is still black.
            vg_sfx_play(SFX_TV_ON, 1.0f);
        }
        return;
    }
    if (vg.tv.t >= TV_IN_TIME) {
        vg.tv.phase = TV_NONE;
        vg.tv.t     = 0.0f;
    }
}
