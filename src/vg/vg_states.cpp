#include "vg_sim.h"
#include "vg_weapons.h"
#include "vg_flight.h"
#include "vg_states.h"
#include "vg_ift.h"
#include "vg_prof.h"
#include <Arduino.h>
#include "vg_arena.h"
#include "vg_anomaly.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_menu.h"
// EVERY SCREEN, because the state machine is the one thing that talks to
// all of them -- it runs their hit tests and hands them a tap. A list this
// long in one file is the right place for it; the same list in a header
// four screens included was not.
#include "vg_select.h"
#include "vg_entry.h"
#include "vg_bracket.h"
#include "vg_repair.h"
#include "vg_pause.h"
#include "vg_ui.h"
#include "vg_draw.h"   // vg_press_set: the live contact, recorded once a frame
#include "vg_save.h"
#include "vg_cine.h"
#include "vg_bot.h"
#include "vg_course.h"
#include "vg_sfx.h"
#include "vg_shake.h"
#include "vg_cockpit.h"
#include "vg_surge.h"
#include "vg_replay.h"
#include "vg_raster.h"
#include "vg_canopy_set.h"
#include "vg_canopy_op.h"
#include <math.h>
#include "vg_canopy_draw.h"
#include "vg_tv.h"
#include "vg_comms.h"

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
// TvState vg_tv and vg_tv_clear moved to vg_tv.cpp, with the clock that drives them.

void vg_use_menu_sky(void) {
    vg_sky_menu();
    // AND A ROUND TUBE, because out of combat the arena is the menu's too. A match can end
    // while an anomaly is at full strength -- the killing shot does not wait for the weather --
    // and without this the bracket, the repair screen and the title would all be flown in the
    // distorted tunnel of a fight that is already over. Here rather than in each of the three
    // enter hooks: this function already means "the venue is not a venue any more", and that is
    // the same sentence. See ANOM_CHANCE_ROUND0.
    vg_anomaly_clear();
    vg_surge_clear();
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

    vg_trail.n     = 0;
    vg_trail.head  = 0;
    vg_trail.acc   = 0;
    vg_cockpit.banner.ev   = MSL_NONE;
    vg_cockpit.banner.t = 0;
    vg_threat.on      = false;
    vg_wpn.target = -1;
    vg_wpn.locked      = false;
    vg_cockpit.flash.hit   = 0;
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
    vg_wall_seed();
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

    // AND THE RACK, WHICH THIS DID NOT TOUCH. Everything else the course inherits
    // from whatever came before is cleared above -- the enemies, the missiles in the
    // air, the debris, the fireballs -- and the weapon was simply missed.
    //
    // The course is reachable from the bracket as well as before the first match, so
    // the state it inherits is a real one: fly a match down to two rounds, start the
    // reload, take the course, and the instrument shows two cells and then a solid
    // bar over the other ten, because the reload clock is still running. Reported
    // from the glass as the missile slots being covered, and it is not a rendering
    // fault at all -- the rack is honestly drawing a state the course forgot to
    // clear.
    //
    // A full rack and no clock, which is what vg_match_start does and for the same
    // reason: nothing here is going to be shot at.
    vg_wpn.rounds   = vg.spec->magazine;
    vg_wpn.reload_t = 0.0f;
    vg_wpn.fire_gap = 0;
    vg_wpn.target   = -1;
    vg_wpn.lock_t   = 0;
    vg_wpn.locked   = false;

    vg.roll      = 0;
    vg.roll_rate = 0;
    vg.bank      = 0;
    // THE COCKPIT FIRST, and everything else in a chain behind it. hud_boot is NOT set here any
    // more, nor is SFX_READY played: the view sits dark, the cockpit arrives a region at a time,
    // and vg_hud_decay cues the instruments off its progress. Starting all three at once is what
    // made them overlap too tightly to read.
    vg_cockpit.cued    = false;
    vg_cockpit.ready       = false;
    vg_cockpit.regions_lit = 0;
    // WHICH COCKPIT, before the sequence that lights it. vg_canopy_use drops the colour table
    // and the warp maps so they rebuild against the new drawing, and vg_canopy_intro_begin sizes
    // itself from the drawing's region count -- so the order here is not cosmetic.
    vg_canopy_use(vg_canopy_for(vg.ship));
    // ...and the opaque bake beside it, for the hull that has one. The band pass
    // prefers this when it is not null -- see vg_canopy_op.h.
    vg_canopy_op_use(vg_canopy_op_for(vg.ship));
    // ...in the player's own colour, if the drawing was baked with a mask for it.
    vg_canopy_op_tint(vg.trail_hue);
    vg_canopy_intro_begin();
    // THE ONLY SOUND THAT PLAYS TO A BLACK SCREEN. It fills the second of dark before the first
    // region lights, so the wait reads as something spooling up rather than as nothing happening --
    // which is the difference between a pause and a hang. Fired here and not from the sequence
    // itself: the sequence lives in the band rasteriser, and a cue triggered from drawing code is
    // a cue that does not happen when the panel is not drawn.
    vg_sfx_play(SFX_SPOOL, 1.0f);
    // THE COURSE IS NOT A MATCH, so it does not go through vg_match_start and has to put the
    // throttle back itself. Both places, or "every time" is only true of one of them.
    vg.throttle     = THROTTLE_START;
    vg.throttle_vis = THROTTLE_START;
    vg_input_throttle_set(THROTTLE_START);
    // THE COURSE IS WHERE THE DISPLACEMENT GETS LOOKED AT, for now. It belongs in a match --
    // the course is where the wall warning is met for free and where the controls are learned,
    // and a boundary that moves is a difficulty rather than a lesson. But a tournament's first
    // round is deliberately the faintest setting in the game, which makes it the worst place to
    // judge the effect, so the course carries it at full strength while the look is decided.
    // Set ARENA_WARP_COURSE to 0 to put the course back to a round tube.
    vg_arena_warp_set(ARENA_WARP_COURSE);
    vg_input_calibrate();
}

// Arriving at the tournament table. Pulled out of the transition's switch so
// that it is the STATE's set-up and not one caller's: the table is reachable
// from the transition, from the repair screen and from the end of a round, and
// each of those used to prepare it differently.
static void enter_bracket(void) {
    // The results the page reads out, composed once here. See vg_bracket_chyron.
    vg_bracket_chyron();
    // The table is a menu, so it gets the menu's backdrop -- it used to keep
    // whatever the last venue built.
    vg_use_menu_sky();
    vg_course.ring_alive = false;
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
    // The cutscene posts the two introductions itself, and a SKIP can leave one
    // of them still on the air. Nothing else takes it down: vg_state_cut clears
    // the broadcast, but INTRO leaves through vg_state_go. Here rather than in
    // vg_upd_intro because the leave hook is the one path every exit takes, and
    // not in vg_cine_clear, which also runs BETWEEN SHOTS and would cut the
    // first introduction off at the cut to the second.
    vg_ift_clear();
    vg.cam_zoom = 1.0f;
}

// Entering the flight, which is nearly always a continuation -- back from a hit,
// back from the pause -- and must stay one. Only a gym rep asks for a whole new
// match here, and only when the latch says so; see Vg::gym_arm.
static void enter_playing(void) {
    if (vg.gym && vg.gym_arm) {
        vg.gym_arm = false;
        vg_gym_start();
    }
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
    vg_wall_seed();
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive  = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive  = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;
    vg_spawn_opponent();

    vg_cockpit.cued    = false;         // the boot chain again -- see enter_course
    vg_cockpit.ready       = false;
    vg_cockpit.regions_lit = 0;
    // WHICH COCKPIT, before the sequence that lights it. vg_canopy_use drops the colour table
    // and the warp maps so they rebuild against the new drawing, and vg_canopy_intro_begin sizes
    // itself from the drawing's region count -- so the order here is not cosmetic.
    vg_canopy_use(vg_canopy_for(vg.ship));
    // ...and the opaque bake beside it, for the hull that has one. The band pass
    // prefers this when it is not null -- see vg_canopy_op.h.
    vg_canopy_op_use(vg_canopy_op_for(vg.ship));
    // ...in the player's own colour, if the drawing was baked with a mask for it.
    vg_canopy_op_tint(vg.trail_hue);
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
    // ...and what this state PUTS ON THE SCREEN instead of the world.
    //
    // This was the fifth hand-kept list the note above is about, and it outlived
    // the other four: a switch in vg_render.cpp with a default arm that drew the
    // overlays. A menu state left out of it did not fail to build and did not
    // draw nothing -- it drew the overlays, which is a picture, so the fault read
    // as the new screen being blank rather than as the screen never having been
    // asked for.
    //
    // So the four states that genuinely want the overlays SAY so, and the default
    // is gone. The static_assert under the table is then what a switch could
    // never have been given: a menu row with nothing in this column does not
    // build.
    //
    // NULL FOR A STATE THAT FLIES, and that is not an omission -- the world is
    // its picture, and the instruments over it belong to the flight path in
    // vg_render.cpp rather than to any one state.
    //
    // PAUSE IS THE EXCEPTION AND STAYS ONE. It is the only screen drawn OVER the
    // frame rather than instead of it -- last of all, over the instruments, after
    // the broadcast has been suppressed -- so it is called by name at the end of
    // the frame. Putting it in this column would mean the column meant two
    // different things depending on the flags beside it.
    void      (*draw)(void);
};

// In enum order. Positional, like the crumb table, so the two read the same way
// side by side.
// CONSTEXPR, so that the check under it can read it. A const array of a
// non-literal type cannot be looked at in a constant expression; this one is an
// aggregate of pointers with constant initialisers, so it can be, and it costs
// nothing it was not already costing -- the table was in flash either way.
static constexpr VgStateDef STATES[VG_STATE_COUNT] = {
    { "ATTRACT",   VGS_MENU | VGS_DRIFT,               enter_attract, nullptr,     vg_upd_attract,   vg_draw_overlays },
    { "ENTRY",     VGS_MENU | VGS_DRIFT,               nullptr,       nullptr,     vg_upd_entry,     vg_draw_entry },
    { "SELECT",    VGS_MENU | VGS_DRIFT,               nullptr,       nullptr,     vg_upd_select,    vg_draw_select },
    { "REPAIR",    VGS_MENU | VGS_DRIFT,               nullptr,       nullptr,     vg_upd_repair,    vg_draw_repair },
    { "BRACKET",   VGS_MENU | VGS_DRIFT,               enter_bracket, nullptr,     vg_upd_bracket,   vg_draw_bracket },
    // THE OVERLAYS ARE A SCREEN HERE, not a fallback. The launch cutscene's
    // fighter cards, the title, the scorecard and the winner all come out of
    // vg_draw_overlays, and four states naming it is the difference between four
    // screens that share a drawing and four screens nobody ever listed.
    { "INTRO",     VGS_MENU,                           enter_intro,   leave_intro, vg_upd_intro,     vg_draw_overlays },
    { "PLAYING",   VGS_LIVE | VGS_ENGINE | VGS_COMBAT, enter_playing, nullptr,     vg_upd_playing,   nullptr },
    { "HIT",       VGS_LIVE | VGS_ENGINE | VGS_COMBAT, nullptr,       nullptr,     vg_upd_playing,   nullptr },
    // Still flying, and that is the whole of it: the opponent is down and
    // talking, the player cannot be hurt, and cutting the hum at that moment
    // would be the loudest thing about it.
    { "KILL",      VGS_ENGINE,                         nullptr,       nullptr,     vg_upd_kill,      nullptr },
    // Nothing. A pause is not a place -- it suspends one. Its screen goes over
    // the suspended frame at the end of vg_render; see the note on draw.
    { "PAUSE",     0,                                  nullptr,       nullptr,     vg_upd_pause,     nullptr },
    { "COURSE",    VGS_LIVE | VGS_ENGINE,              enter_course,  nullptr,     vg_upd_course,    nullptr },
    { "ROUND_WON", VGS_MENU | VGS_DRIFT,               nullptr,       nullptr,     vg_upd_round_won, vg_draw_overlays },
    { "OVER",      VGS_MENU,                           nullptr,       nullptr,     vg_upd_over,      vg_draw_overlays },
    { "WON",       VGS_MENU | VGS_DRIFT,               nullptr,       nullptr,     vg_upd_won,       vg_draw_overlays },
};

static_assert(sizeof(STATES) / sizeof(STATES[0]) == VG_STATE_COUNT,
              "a state was added without a row, or a row without a state");

// EVERY MENU STATE HAS A PICTURE.
//
// This is the check the switch could not be given. A menu is a state that
// replaces the world, so a menu with nothing in the draw column is a state that
// shows the player whatever the renderer happened to leave behind.
//
// Recursive rather than a loop, because it has to hold on the firmware toolchain
// too and a constexpr function there is one return statement.
static constexpr bool vg_states_all_drawn(int i) {
    return i >= VG_STATE_COUNT
        || (((STATES[i].flags & VGS_MENU) == 0 || STATES[i].draw != nullptr)
            && vg_states_all_drawn(i + 1));
}
static_assert(vg_states_all_drawn(0),
              "a menu state has nothing in its draw column");
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

void vg_state_resume(VgState to, float t) {
    if ((int)to >= VG_STATE_COUNT) return;
    vg.state   = to;
    vg.state_t = t;
}

void vg_state_cut(VgState to) {
    if (vg_tv.phase != TV_NONE) return;   // one transition at a time
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
    vg_tv_begin((uint8_t)to);
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
// The screen a state puts up, or nothing at all if it flies. Called from the
// menu branch of vg_render, which is the one place that asks.
void vg_state_draw(VgState s) {
    if ((int)s >= VG_STATE_COUNT) return;
    const VgStateDef* d = &STATES[s];
    if (d->draw) d->draw();
}

VgState vg_state_shown(void) {
    if (vg.state != VG_PAUSE) return vg.state;
    return ((int)vg.pause_from < VG_STATE_COUNT) ? (VgState)vg.pause_from
                                                 : VG_PLAYING;
}

void vg_state_update(float dt, const VgInput* in, const Tap* tap) {
    // Before the state runs, so a control drawn this frame reports the contact
    // that is happening now rather than the one from the frame before.
    vg_press_set(in && in->menu_held, in ? in->menu_x : 0.0f,
                 in ? in->menu_y : 0.0f);
    if ((int)vg.state >= VG_STATE_COUNT) return;
    const VgStateDef* d = &STATES[vg.state];
    if (d->update) d->update(dt, in, tap);
}

static void tv_join(uint8_t to) {
    vg_state_go((VgState)to);
}

void vg_tv_update(float dt) {
    // The transition owns its clock; the join is a state change and that is ours.
    vg_tv_step(dt, tv_join);
}

// ===========================================================================
// THE STATE HANDLERS
//
// One function per state, moved here from vg_game.cpp where they sat beside the
// spawning, the collisions and the match lifecycle. The table above is in this
// file and every row's `update` column points into what follows -- so the rows
// and the functions they name are now readable side by side instead of a file
// apart.
//
// collide_player and the purse came with them because nothing else calls either:
// the collision is reached from vg_upd_course and vg_upd_playing, and the purse
// from vg_upd_kill.
// ===========================================================================

// ---------------------------------------------------------------------------
// Collisions
// ---------------------------------------------------------------------------

static void collide_player(void) {
    // Boundary contact is fatal, so there is no bouncing the player back inside
    // any more -- the run is simply over.
    if (vg_wall.clearance < SHIP_RADIUS) {
        // The player's own airframe against the boundary. Right on top of the
        // cockpit, so the flash vg_spawn_blast raises off the range is the
        // brightest one in the game -- which is correct for hitting a wall.
        vg_spawn_blast(v3(0, 0, 14), 34.0f, 7, 0, 1.7f);
        vg_spawn_shrapnel(v3(0, 0, 14), 26.0f, 40.0f, 30, 4.0f, 1.7f);
        vg_kill_player();
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        Asteroid* a = &vg.ast[i];
        if (!a->alive) continue;
        float r = a->radius + SHIP_RADIUS;
        if (vlen2(a->pos) < r * r) {
            vg_spawn_debris(a->pos, a->radius, 8);
            a->alive = false;
            vg_kill_player();
        }
    }

    // Merging head-on has to cost something, or flying straight through them
    // becomes a free way to reverse the geometry.
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        float r = ENEMY_HIT_RADIUS + SHIP_RADIUS;
        if (vlen2(s->pos) < r * r) {
            // Say it BEFORE clearing alive. A pilot who dies in a collision gets
            // the same last line as one who dies to a missile; only the missile
            // path used to speak, so half the deaths in the game were silent.
            vg_comms_say(s, VOICE_DEATH);
            // A ram kills them too, and it used to be twelve shards and a radio
            // line. Same eruption a missile kill gets: the hull does not care
            // what opened it.
            vg_spawn_blast(s->pos, 46.0f, 9, 0, 1.9f);
            vg_spawn_shrapnel(s->pos, 30.0f, 54.0f, 34, 4.4f, 1.8f);
            s->alive = false;
            // DAMAGE, NOT AN OUTCOME. See SHIP_COLLIDE_DAMAGE: a ram is meant to
            // be a desperate trade, and it stopped being one when it killed
            // whatever it touched regardless of the hull it hit.
            vg_damage_player(vg.health_max * SHIP_COLLIDE_DAMAGE);
        }
    }
}

// ---------------------------------------------------------------------------
// Update

// Purse for the round just won, plus a condition bonus scaled by hull remaining.
// Flying well therefore pays twice -- you earn more AND need less repair -- but
// the flat base dominates, so one scrappy match does not spiral the run.
//
// Called BEFORE vg_tourney_resolve, which is what advances the round counter.
static int s_last_purse = 0;

static void award_purse(void) {
    static const int BASE[TOURNEY_ROUNDS] = {
        CREDIT_R16, CREDIT_QF, CREDIT_SF, CREDIT_FINAL
    };
    const int r    = (vt.round < TOURNEY_ROUNDS) ? vt.round : TOURNEY_ROUNDS - 1;
    const int base = BASE[r];

    float cond = (vg.health_max > 0.0f) ? (vg.health / vg.health_max) : 0.0f;
    if (cond < 0.0f) cond = 0.0f;
    if (cond > 1.0f) cond = 1.0f;

    s_last_purse = base + (int)((float)base * CREDIT_CONDITION_K * cond + 0.5f);

    vg.credits += s_last_purse;
    if (vg.credits > CREDIT_CAP) vg.credits = CREDIT_CAP;

    // Banked immediately. Losing a purse to a flat battery between the kill and
    // the next screen would be the single most annoying way to lose progress.
    vg_save_store();
}

int vg_last_purse(void) { return s_last_purse; }

// ---------------------------------------------------------------------------
// One function per state
//
// Each is a row's `update` column. They take the frame, the gated input and the
// resolved tap, and NOTHING ELSE -- checked before splitting them out, because a
// case that reached for one of the dispatch's own locals would have needed a
// wider signature and a worse one. None did.
// ---------------------------------------------------------------------------

void vg_upd_attract(float dt, const VgInput* in, const Tap* tap) {
#if VG_BENCH
    // Synthetic worst case: a full complement of fighters, all manoeuvring,
    // trailing and shooting, plus the player's own rack cycling. Reproduces
    // the primitive load of a heavy match without anyone touching the board.
    {
        int alive = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) if (vg.enemy[i].alive) alive++;
        if (alive < MAX_ENEMIES) {
            for (int i = 0; i < MAX_ENEMIES; i++)
                if (!vg.enemy[i].alive) {
                    vg_spawn_enemy(i, (ShipClass)(i % SHIP_CLASSES), ENEMY_SKILL,
                                0.1f + 0.45f * (float)i, vg_pilot_default());
                    break;
                }
        }
        for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);
        vg_update_lock(dt);
        if (vg_wpn.fire_gap > 0) vg_wpn.fire_gap -= dt;
        vg_update_reload(dt);
        if (vg_wpn.locked) vg_player_fire();
        vg.health = vg.health_max;      // never let the load generator "die"
    }
#endif
    // THE GYM IS A CHORD, AND IT IS NOT ADVERTISED.
    //
    // Hold the alt button and tap to start. The title screen says TOUCH TO START
    // and says nothing else: this is a workshop door, for whoever is tuning
    // combat, and a player who came for the tournament will never find it by
    // doing what the screen tells them.
    //
    // A CHORD RATHER THAN A BUTTON OF ITS OWN, because the alt button alone is
    // reachable by accident -- it is the roll control in flight, so a hand
    // already rests on it -- and a stray press that swallowed the start of a run
    // would be a bug report about the tournament refusing to open.
    //
    // Tested BEFORE the plain tap and returning, or the same tap would open the
    // gym and then be spent again on the entry screen behind it.
    if (tap->up && in->roll_btn) {
        vg.gym     = true;
        vg.sel_opp = false;
        vg_state_cut(VG_SELECT);        // the workshop door is a door too
        return;
    }

    if (tap->up) {
        vg.gym = false;      // the tournament door clears the workshop
        vg_entry_reset();
        // A CUT, NOT A GO, and the transition is doing real work here rather
        // than decorating the change. The title is a starfield and what comes
        // next is a terminal in a room: the set goes off, holds a second of dead
        // air, and opens back up on the console. Without it the machine appears
        // over the stars in one frame and the two places are the same place.
        vg_state_cut(VG_ENTRY);
        return;
    }

    // THE TITLE SCREEN DOES NOT PLAY THE GAME TO ITSELF ANY MORE.
    //
    // It did, and it worked, and it is the wrong thing to ship: what a player
    // wants from this game is a fight that is hard and worth playing, and combat
    // footage of nobody playing is not that. Author's call.
    //
    // The demo itself is kept and reachable -- vg_demo_begin, and --demo on the
    // desktop -- because it is still the only way to watch the trained pilot fly
    // the player's seat on the board, where there is no command line to ask with.
    if (vg_demo_wanted && vg.state_t > DEMO_AFTER && vg_tv.phase == TV_NONE)
        vg_demo_begin();
}

void vg_upd_entry(float dt, const VgInput* in, const Tap* tap) {
    if (vg_entry_update(in, tap->up, tap->x, tap->y)) {
        vg_state_go(VG_SELECT);
    }
}

void vg_upd_repair(float dt, const VgInput* in, const Tap* tap) {
    if (vg_repair_update(in, tap->up, tap->x, tap->y)) vg_state_go(VG_BRACKET);
}

// THE SHIP WHEEL, and it is the CALLSIGN WHEEL -- same gesture, same step, same
// direction. See vg_entry_update, which this deliberately mirrors.
//
// Three ways in, because a wheel that only answers to a drag is a wheel the
// hardware button cannot reach: drag it, tap a neighbour to nudge it by one, or
// press the alt key to cycle.
static int   s_wheel_accum_owner = 0;   // non-zero while a drag owns the wheel

// Moving the selection is TWO DIFFERENT WRITES and they must not be confused.
// vg_game_select_ship also resets the player's hull and rack, so using it to
// choose an OPPONENT would silently re-arm the player as the class they picked
// to fight. That bug is why this helper exists rather than being inlined.
static void select_step(int delta) {
    if (vg.gym && vg.sel_opp) {
        vg.gym_opp = (uint8_t)((vg.gym_opp + delta + SHIP_CLASSES * 4) % SHIP_CLASSES);
    } else {
        const int n = ((int)vg.ship + delta + SHIP_CLASSES * 4) % SHIP_CLASSES;
        vg_game_select_ship((ShipClass)n);
    }
}

void vg_upd_select(float dt, const VgInput* in, const Tap* tap) {
    (void)dt;

    // The +/- key cycles as well, since it is already wired and is the fastest
    // way to feel the difference between classes.
    if (in->alt_edge) select_step(+1);

    // --- the wheel ---------------------------------------------------------
    if (in->menu_edge) {
        vg_wheel_release(vg_select_wheel());
        s_wheel_accum_owner =
            (vg_select_row_at(in->menu_x, in->menu_y) != VG_WHEEL_NONE);
    }
    if (in->menu_held) {
        if (s_wheel_accum_owner) {
            // The direction and the detent size are the wheel's -- see VgWheel.
            // This end of it is only the part that is a change to the GAME.
            const int d = vg_wheel_drag(vg_select_wheel(), in->menu_dy);
            for (int i = 0; i < (d < 0 ? -d : d); i++) select_step(d > 0 ? +1 : -1);
        }
    } else {
        s_wheel_accum_owner = 0;
    }

    if (tap->up) {
        const int row = vg_select_row_at(tap->x, tap->y);
        if (row != VG_WHEEL_NONE) {
            // A tap above or below the detent nudges by one, so the wheel is
            // usable without a drag at all. A tap ON the detent is a no-op: it
            // is already the selection.
            if (row != 0) select_step(row);
        } else if (vg_select_confirm_at(tap->x, tap->y)) {
            if (vg.gym) {
                // TWO PASSES THROUGH ONE SCREEN. The first confirm keeps the
                // player's ship and comes straight back for the opponent's,
                // which is why this does not leave the state: the wheel, the
                // chart and the hit tests are the same question asked twice.
                if (!vg.sel_opp) {
                    vg.sel_opp = true;
                    // The second pick starts on the ship just chosen rather than
                    // wherever the wheel was left, so confirming twice is a
                    // mirror match -- the most common thing to want.
                    vg.gym_opp = (uint8_t)vg.ship;
                } else {
                    vg.gym_arm = true;
                    vg_state_cut(VG_PLAYING);
                }
                return;
            }
            // The draw is made HERE, so the course that follows has a
            // tournament to return to and the player flies it in the airframe
            // they actually picked.
            vg_tournament_begin(vg.ship);
            vg_bracket_focus_player();
            vg_state_cut(VG_COURSE);
        }
    }
}

void vg_upd_intro(float dt, const VgInput* in, const Tap* tap) {
    // The shot itself lives in vg_cine.cpp. What belongs here is only the
    // handover: the cutscene has spent seventeen seconds turning and
    // drifting the viewpoint for the sake of framing, and relocating
    // between setups moved it again -- so where it ended up relative to the
    // arena is not something a match should have to inherit. Both the arena
    // and the opponent are re-established from scratch, which makes a bad
    // start structurally impossible rather than merely unlikely.
    //
    // Free to do because the handover is a hard cut to black with the
    // instruments rebooting over it. None of the snap is visible.
    // The broadcast introduces each fighter over its own shot. The cutscene
    // already hard-cuts between them, so the cues are the shot boundaries.
    if (vg.state_t > INTRO_DRIFT && !(vg_bcast.ift_fired & (1u << IFT_INTRO_YOU))) {
        vg_bcast.ift_fired |= (1u << IFT_INTRO_YOU);
        vg_ift_line(IFT_INTRO_YOU);
    }
    if (vg.state_t > INTRO_OPP_START && !(vg_bcast.ift_fired & (1u << IFT_INTRO_OPP))) {
        vg_bcast.ift_fired |= (1u << IFT_INTRO_OPP);
        vg_ift_line(IFT_INTRO_OPP);
    }

    if (vg_cine_update(dt, tap->up)) {
        // Both halves have names now. The cutscene's own teardown is INTRO's
        // leave hook and runs inside vg_state_go; the match set-up is
        // vg_begin_flight, which is called by name because VG_HIT re-enters
        // VG_PLAYING and must not repeat any of it.
        vg_begin_flight();
        vg_state_go(VG_PLAYING);
    }
}

void vg_upd_course(float dt, const VgInput* in, const Tap* tap) {
    // Flying, and nothing else. No opponent, no missiles, no purse. The wall
    // is still lethal because that is one of the things worth learning here.
    vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);
    vg_course_update(dt);
    collide_player();

    if (vg_player_was_hit()) {
        // A crash costs the streak and nothing else. Full hull, back in the
        // tube, count at zero -- this is a practice range, and ending a
        // tournament that has not started would be absurd.
        vg_clear_player_hit();
        vg.health      = vg.health_max;
        vg_course.hits = 0;
        vg_arena_init(ARENA_TORUS);
        vg_wall_seed();
        vg_ift_line(IFT_COURSE_MISS);
        vg_course_reset_streak();
    }

    // Finishing is a MOMENT, not an exit condition. The gate is already gone
    // and the player keeps flying while the broadcast marks it, exactly as a
    // kill does -- see COURSE_DONE_BEAT.
    if (vg_course.done) {
        vg_course.end_t += dt;
        if (vg_course.end_t > COURSE_DONE_BEAT) vg_state_cut(VG_BRACKET);
    }


}

void vg_upd_kill(float dt, const VgInput* in, const Tap* tap) {
    // The world keeps running -- wreckage still tumbles, their last trail
    // still fades -- but nothing can touch the player. The only job of this
    // state is to let the dead pilot finish talking.
    vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);
    vg_update_missiles(dt);
    vg_update_threat();
    if (vg_wpn.fire_gap > 0) vg_wpn.fire_gap -= dt;
    // After the last transmission, not over it. KILL_SPEECH is exactly how
    // long the dying pilot holds the other slot, so this lands in the silence
    // that follows and runs on into the bracket redraw.
    // NOT IN THE GYM. The line names the round and the opponent out of the
    // BRACKET, and a gym has neither -- vg_tourney_opponent would hand back
    // whichever entrant the last tournament left indexed, so the announcer would
    // sum up a match that is not being played.
    if (!vg.gym &&
        vg.state_t > KILL_SPEECH && !(vg_bcast.ift_fired & (1u << IFT_MATCH_END))) {
        vg_bcast.ift_fired |= (1u << IFT_MATCH_END);
        vg_ift_line(IFT_MATCH_END);
    }
    if (vg.state_t > KILL_BEAT) {
        if (vg.gym) {
            // NO PURSE AND NO TRANSITION. The player is still flying, still
            // holding whatever hull the last fight cost them, and only the
            // target needs replacing -- so the next one simply arrives down the
            // tunnel the way the first did. Cutting to black here would make a
            // rep cost four seconds of dead air, and the whole value of a gym is
            // how fast it comes round again.
            vg_gym_spawn_opponent();
            vg_state_go(VG_PLAYING);
            return;
        }
        award_purse();
        vg_state_go(VG_ROUND_WON);
    }
}

void vg_upd_bracket(float dt, const VgInput* in, const Tap* tap) {
    if (in->menu_held) vg_bracket_pan(in->menu_dx, in->menu_dy);
    if (tap->up) {
        if (vg_bracket_ready_at(tap->x, tap->y)) {
            vg_state_cut(VG_INTRO);
        } else if (vg_bracket_course_at(tap->x, tap->y)) {
            vg_state_cut(VG_COURSE);
        } else if (vg_bracket_repair_at(tap->x, tap->y)) {
            vg_repair_reset();
            vg_state_go(VG_REPAIR);
        }
    }
}

void vg_upd_pause(float dt, const VgInput* in, const Tap* tap) {
    // No world step: paused means paused.
    const bool from_course = (vg.pause_from == VG_COURSE);
    (void)dt;

    // PWR again, which is what paused it. NOT the + key -- that is the roll
    // control, and a paused player leaning on it would be thrown back into
    // the fight mid-roll.
    if (vg.pause_page == 1) {
        // Dragged, not tapped: held rather than on release, so the fill
        // follows the finger instead of jumping when it lifts.
        if (in->menu_held) {
            if (vg_pause_music_at(in->menu_x, in->menu_y))
                vg_vol.music = vg_pause_slider_value(in->menu_x);
            else if (vg_pause_sfx_at(in->menu_x, in->menu_y))
                vg_vol.sfx = vg_pause_slider_value(in->menu_x);
        }
        // TAPPED, not dragged, unlike the sliders above: a checkbox is a
        // decision and a slider is an adjustment, and holding a finger on a
        // decision should not flip it forty times.
        if (tap->up && vg_pause_scanline_at(tap->x, tap->y)) {
            vg_disp.scanlines = !vg_disp.scanlines;
            vg_save_store();
            return;
        }
        if (tap->up && vg_pause_back_at(tap->x, tap->y)) {
            vg.pause_page = 0;
            vg_save_store();        // settings outlive the session
        }
        return;   // was a break out of the switch: this frame is done
    }

    if (tap->up) {
        switch (vg_pause_item_at(tap->x, tap->y, from_course)) {
        case PAUSE_RESUME:
            vg_state_resume((VgState)vg.pause_from, vg.pause_t);
            break;
        case PAUSE_CONFIG:
            vg.pause_page = 1;
            break;
        case PAUSE_SKIP:
            // Refused while the briefing runs: the player may pause over it,
            // read it and think about it, but not walk out of it.
            if (vg_course.named) vg_state_cut(VG_BRACKET);
            break;
        case PAUSE_QUIT:
            // Cleared here rather than in enter_attract, which the attract screen
            // also reaches by other routes. Walking out of the workshop is the
            // one act that ends a gym session.
            vg.gym     = false;
            vg.gym_arm = false;
            vg_state_cut(VG_ATTRACT);
            break;
        default: break;
        }
    }
}

void vg_upd_round_won(float dt, const VgInput* in, const Tap* tap) {
    if (vg.state_t <= 2.4f) return;

    // RESOLVING IS ONCE, AND IT HAS TO BE SAID OUT LOUD NOW.
    //
    // vg_tourney_resolve advances the round counter AND simulates every other
    // match in the draw, so calling it twice is a second tournament: the player
    // skips a round and the rest of the bracket is re-rolled underneath them.
    //
    // It was safe when this was a vg_state_go, which changed the state on the
    // same frame and so could not come back here. A cut does not: the state
    // does not change until the JOIN, most of two seconds later, and this
    // handler keeps running for every frame of the set going out. Hence the
    // phase test -- the transition being up IS the record that the draw has
    // already been resolved.
    if (vg_tv.phase != TV_NONE) return;

    vg_tourney_resolve(true);
    if (vt.complete) {
        vg.champion = true;
        // Back out of combat, so back to the menu sky. The bracket
        // gets this from enter_bracket; the winner's card does not
        // pass through it.
        vg_use_menu_sky();
        vg_state_go(VG_WON);
        vg_save_store();      // the name sticks from here on
    } else {
        // THE SET GOES OFF BETWEEN THE FIGHT AND THE TABLE. Every other way
        // into the bracket is a cut -- finishing the course, walking out of a
        // briefing -- and winning a match was the one path that snapped
        // straight there, which read as the scorecard being replaced rather
        // than as the broadcast moving on.
        //
        // It also puts enter_bracket behind the dead air, where the rest of the
        // game already puts its set-up: the table is now built while the screen
        // is black and the aperture opens onto a finished draw, instead of the
        // redraw happening in front of the player.
        //
        // NOTHING OF THE MATCH IS STILL TALKING BY HERE. IFT_MATCH_END goes up
        // KILL_SPEECH into the wreck and holds for IFT_SPEECH, which runs out
        // 1.8s into this state's 2.4 -- so the vg_ift_clear inside the cut has
        // nothing to take off the air, and the line still gets its full read.
        vg_state_cut(VG_BRACKET);
    }
}

void vg_upd_won(float dt, const VgInput* in, const Tap* tap) {
    // Returns on its own. The sequence ends by handing the player back to
    // the title card, where the crawl now says the rumour is about them --
    // so the payoff is not the win screen, it is the menu behind it having
    // quietly changed. A prompt would invite a tap that skips exactly that.
    //
    // Tapping is allowed only once the name is fully up, so an impatient
    // hand cannot cut the one moment the whole story was built toward.
    //
    // A CUT, not the enter hook. This called vg_enter_attract() directly, which is
    // ATTRACT's own entry in the state table -- so it built the attract scene and
    // never changed vg.state. The game stayed in WON for ever: the title card had no
    // crawl, because the crawl belongs to ATTRACT, and nothing answered a tap,
    // because vg_upd_won answers nothing else. Beating the game bricked the session.
    //
    // vg_state_cut is what VG_OVER already uses for the same journey, and it is safe
    // to call every frame -- it returns early while a transition is running, which
    // matters here because this condition stays true until the state actually changes.
    if (vg.state_t > WON_RETURN ||
        (vg.state_t > WON_NAME_IN + 2.6f && tap->up)) vg_state_cut(VG_ATTRACT);
}

void vg_upd_playing(float dt, const VgInput* in, const Tap* tap) {
    const bool playing = (vg.state == VG_PLAYING);

    vg_clear_player_hit();

    vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);

    // BEFORE THE AI AND THE COLLISIONS, and after the world has moved. The amplitude this sets
    // is the arena both of them are about to be tested against, so setting it later would spend
    // a frame steering and colliding against last frame's boundary. At an onset ramp of a
    // couple of seconds that is invisible, and it is still wrong.
    //
    // FROZEN ONCE THE OPPONENT IS DOWN. VG_KILL is the broadcast's own moment -- the loser
    // transmits and then the IFT sums up the round -- and the match is decided by then, so the
    // weather has nothing left to say. Stepped through VG_HIT, which is right: the arena does
    // not care that the player was hit. Called from here rather than vg_upd_*, so it holds still
    // during VG_PAUSE for free, that function not being called there.
    vg_anomaly_step(dt, vg.state == VG_KILL);
    vg_surge_step(dt, vg.state == VG_KILL);

    const uint32_t t_ai = micros();
    for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);
    const uint32_t t_cbt = micros();
    g_upd_ai += t_cbt - t_ai;

    // After they have moved, or the range being tested is a frame stale --
    // which at a combined 800 units a second is sixteen units of error.
    vg_update_passes();

    vg_update_missiles(dt);
    vg_update_lock(dt);
    vg_update_threat();
    g_upd_combat += micros() - t_cbt;

    // No hull regeneration. Damage taken here is carried for the rest of the
    // tournament and only credits will undo it, which is what makes the
    // repair economy the difficulty curve rather than a side system.

    if (vg_wpn.fire_gap > 0) vg_wpn.fire_gap -= dt;
    vg_update_reload(dt);
    if (in->fire_edge) vg_player_fire();

    // Unprompted chatter, on a long timer and only when the radio is idle.
    // Taunts are flavour; letting one interrupt a hit or a kill would turn
    // the most informative channel on the HUD into noise.
    //
    // AND NOT UNTIL THE PANEL IS LIT. The countdown is held, not merely suppressed: an
    // opponent's opening line is the first thing you learn about them, and it was landing
    // while the cockpit was still coming online -- so it played to a dark screen and the
    // timer had already moved on by the time there was a panel to read it on. Held, the
    // opening remark arrives on a lit panel however long the boot took.
    if (vg_cockpit.ready) {
        vg.taunt_t -= dt;
        if (vg.taunt_t <= 0.0f) {
            vg.taunt_t = vg_frand(12.0f, 21.0f);
            if (vg_bcast.ch[BC_PILOT].t <= 0.0f) {
                for (int i = 0; i < MAX_ENEMIES; i++)
                    if (vg.enemy[i].alive) { vg_comms_say(&vg.enemy[i], VOICE_TAUNT); break; }
            }
        }
    }

#if ENABLE_ASTEROIDS
    // Keep the field topped up as a speed cue.
    int alive_ast = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) if (vg.ast[i].alive) alive_ast++;
    vg.spawn_t -= dt;
    if (alive_ast < AST_TARGET_COUNT && vg.spawn_t <= 0) {
        vg_spawn_asteroid();
        vg.spawn_t = vg_frand(0.5f, 1.4f);
    }
#endif

    if (playing) {
        collide_player();

        // Player death is resolved FIRST, which is what makes a mutual kill
        // a loss: you died, so you do not advance, regardless of whether the
        // opponent went down in the same frame.
        if (vg_player_was_hit()) {
            vg_state_go((vg.health > 0.0f) ? VG_HIT : VG_OVER);
            // THE TITLE CHANGES HANDS, and the player stops being anybody.
            //
            // Dying as the champion is not a lost round, it is the end of a legend.
            // The narrative has no room for a PHANTOM who is still flying around
            // having been killed, so the run does not resume from a worse position --
            // it starts again as somebody else, with nothing.
            //
            // WHOEVER KILLED YOU TAKES THE NAME. Attributed to the round's opponent
            // rather than to the specific round or collision that did it, because a
            // tournament match is one pilot against one pilot: if you did not come out
            // of it, they did, and the reason is between you and the wall. That also
            // means a kamikaze, a stray missile and a boundary hit all settle the same
            // way, which is the same rule the bracket already uses.
            // NOT IN THE GYM, AND THIS IS THE IMPORTANT ONE. vg_title_lost
            // strips the title, empties the bank, resets the callsign and the
            // ship, and WRITES ALL OF THAT TO THE SAVE. Dying in a workshop is
            // meant to cost nothing, so without this guard a champion who came
            // in to feel out a CHARIOT would lose the run to a test.
            if (!vg.gym && vg.state == VG_OVER && vg.champion) vg_title_lost();
            if (vg.state == VG_OVER) {
                // Your own ship, left drifting just ahead of the camera.
                // There is no third-person view in a renderer where the
                // player IS the origin, so the wreck is placed in front and
                // the camera set tumbling around it -- which reads as being
                // thrown clear, and gives the scene something to be about.
                Ship* c = &vg_cine.ship;
                c->alive    = true;
                c->spec     = vg.spec;
                c->hue      = vg.trail_hue;
                c->pos      = v3(vg_frand(-70.0f, 70.0f),
                                 vg_frand(-50.0f, 50.0f), 640.0f);
                c->fwd      = vnorm(v3(0.35f, 0.12f, 1.0f));
                c->up       = v3(0, 1, 0);
                c->speed    = 0.0f;
                c->scale    = 84.0f;
                c->roll_vis = 0.4f;
                c->trail.n  = 0;
                c->hit_flash = 0.0f;
                vg_cine.on  = true;
                // The player's own wreck, and the biggest eruption in the
                // game. Scaled off c->scale (84) rather than a ship's own
                // size: this one is deliberately staged large and close, and
                // an explosion tuned for a distant fighter looked like a
                // spark next to it.
                vg_spawn_blast(c->pos, 62.0f, 11, 0, 2.2f);
                vg_spawn_shrapnel(c->pos, 40.0f, 72.0f, 44, 4.8f, 2.0f);
            }
            return;   // was a break out of the switch: this frame is done
        }

        bool opponent_alive = false;
        bool opponent_met   = false;
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (vg.enemy[i].alive) opponent_alive = true;
            if (vg.enemy[i].engaged) opponent_met = true;
        }

        // An opponent who dies without ever reaching the player does not end
        // the match. Send the next one instead.
        //
        // Nothing should reach this now: the distance cull that deleted them
        // is gone, and the only remaining ways to die are a missile and a
        // collision, both of which require the two ships to be together. It
        // stays because the failure it prevents is the worst kind -- a
        // tournament round won without a fight, with no wreck and nobody
        // saying anything, which reads as a broken game rather than a
        // lucky one.
        if (!opponent_alive && !opponent_met) {
            vg_spawn_opponent();
            opponent_alive = true;
        }

        if (!opponent_alive) {
            // Not straight to the scorecard. They are still talking, and
            // cutting to a purse over the top of a dying pilot is the whole
            // difference between a tournament and a spreadsheet.
            vg_state_go(VG_KILL);
        }
    } else if (vg.state_t > 1.2f) {
        vg_state_go(VG_PLAYING);
    }
}

void vg_upd_over(float dt, const VgInput* in, const Tap* tap) {
    // Dead men do not steer. Input is ignored entirely and the camera
    // tumbles on all three axes around the wreck it was thrown from --
    // slowly, and slowing further, like something that has stopped being a
    // ship and started being debris.
    const float decay = 1.0f / (1.0f + vg.state_t * 0.55f);
    vg_world_step(dt,
               0.16f * decay * sinf(vg.state_t * 0.63f),
               0.21f * decay * sinf(vg.state_t * 0.41f + 1.1f),
               0.30f * decay * dt,
               0.0f);
    vg_update_missiles(dt);

    // The image never settles. Camera jitter under the screen-space tearing
    // gives the failure somewhere physical to come from -- one alone reads
    // as an effect, the two together read as a machine coming apart.
    vg_shake.x += vg_frand(-3.4f, 3.4f);
    vg_shake.y += vg_frand(-3.4f, 3.4f);

    // IN THE GYM, DYING IS A REP AND NOT AN ENDING. Same wreck, same tumble, same
    // beat before anything is accepted -- being killed should still read as being
    // killed -- and then a whole new fight instead of the title card.
    //
    // On the same tap, so it is unmistakably the player asking. A gym that
    // respawned on a timer would take the moment away from them, and watching
    // your own wreck is how you work out what went wrong.
    //
    // UNLESS THERE IS NOBODY TO ASK. With the seat flown by the game there is no
    // tap coming, ever, so the rule that gives the player their moment became a
    // rule that ended the session: the first death parked it on the wreck for
    // good. Found by counting -- a headless run of a thousand simulated seconds
    // produced twenty seconds of flying and then nothing.
    if (vg.gym) {
        if (vg.state_t > 2.2f && (tap->up || vg_bot_on)) {
            vg_cine_clear();
            vg.gym_arm = true;
            vg_state_cut(VG_PLAYING);
        }
        return;
    }

    // Knocked out is knocked out: back to the main menu, not a restart.
    if (vg.state_t > 2.2f && tap->up) { vg_cine_clear(); vg_state_cut(VG_ATTRACT); }
}
