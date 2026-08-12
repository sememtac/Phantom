#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"
#include "vg_input.h"
#include "vg_ship.h"

// The whole simulation lives in VIEW SPACE: the player is permanently at the
// origin looking down +z, and the world rotates and translates around it. That
// removes the camera matrix entirely, keeps coordinates small no matter how far
// you fly, and makes "is it in front of me" a plain z test.
//
// A useful consequence for combat: in this frame the player really is
// stationary, so an enemy missile pursuing the player needs no lead prediction
// at all -- pure pursuit toward the origin is already a correct intercept.

struct AstModel {
    Vec3    v[AST_VERTS];
    // Triangles, wound so the cross product of the first two edges points
    // outward. That lets the renderer decide facing with one dot product.
    uint8_t f[AST_FACES][3];
    uint8_t face_count;
};

struct Asteroid {
    bool     alive;
    Vec3     pos;
    Vec3     vel;
    float    radius;
    float    spin[3];
    float    spin_rate[3];
    uint8_t  model;
};

// An enemy fighter. Orientation is carried as forward+up rather than a matrix
// so that repeatedly folding in the world rotation cannot accumulate shear --
// the pair is re-orthonormalised every frame, which a matrix would not be.
struct Ship {
    bool  alive;
    // Which class this fighter is flying. Everything about its performance comes
    // from here, so an NPC BALLISTA is the same ship the player can pick.
    const ShipSpec* spec;
    // Pilot quality, as a scale on turn rate. The bracket will eventually seed
    // this per entrant; for now it is a constant that reproduces the difficulty
    // the game had when enemies were strictly worse than the player.
    float skill;
    Vec3  pos;
    Vec3  fwd;
    Vec3  up;
    float speed;
    float target_speed;
    float hull;           // absolute points, counted down from spec->hull
    float fire_cd;
    float evade_t;        // >0 while breaking away from an incoming missile
    Vec3  evade_dir;
    float break_t;        // >0 while extending away after a firing pass
    Vec3  break_dir;
    Vec3  offset_dir;     // stable lateral offset for the aim point
    float roll_vis;       // visual bank, radians, applied at render time
    float hit_flash;

    // Closest-approach tracking for the pass knock -- see PASS_RANGE. `pass_done`
    // stops one pass being felt on every frame of the separation: it arms while
    // the range is shrinking and fires once, on the frame it starts opening.
    float pass_range;
    bool  pass_done;

    // True once the player and this ship have actually been in the same fight:
    // close enough to see, or hit. A match must not be decided by an opponent
    // who dies before this is set, because the player was never given a chance
    // to be part of it.
    bool  engaged;

    // Suicide run. `kamikaze_will` is this pilot's disposition, rolled once at
    // spawn. `kamikaze_on` is the commitment, and once it is set it never
    // clears: a pilot who has decided to ram does not change their mind.
    bool  kamikaze_will;
    bool  kamikaze_on;

    // Who is flying it. Carried on the ship rather than looked up through the
    // bracket, because a missile detonating has to name the pilot it just
    // killed and has no business knowing what a tournament is.
    char    tag[4];
    uint8_t voice;

    // Model size. Combat ships all fly at ENEMY_SCALE; the cutscene ship is
    // blown up so a hero shot can be framed without putting the camera inside
    // the near plane. At combat scale a fighter is about five pixels across at
    // any distance you could safely hold it, which is a contact, not a subject.
    float   scale;

    // Identity, and the ribbon that carries it. trail_p is the throttle setting
    // each point was laid down at, 0..255 -- what makes the contrail lengthen
    // under power and persist after the ship has backed off.
    float   hue;
    float   trail_acc;
    uint8_t trail_n;
    uint8_t trail_head;
    uint8_t trail_p[SHIP_TRAIL];
    Vec3    trail[SHIP_TRAIL];
};

// What a player missile did, shown briefly in the middle of the screen. A
// proximity-fused seeker is otherwise ambiguous -- a detonation nearby looks
// identical whether it took a third of their hull or nothing at all.
enum MslEvent : uint8_t {
    MSL_NONE = 0,
    MSL_MISSED,
    MSL_HIT,
    MSL_DESTROYED
};

// How long an outcome holds. Shortened when others are waiting, so a burst of
// four resolves in about two and a half seconds rather than five.
#define MSL_BANNER      1.10f
#define MSL_BANNER_FAST 0.62f

struct Missile {
    bool    alive;
    bool    from_player;
    bool    locked;       // false once the seeker has broken lock: goes ballistic
    // The warhead that launched this round. A pointer into static storage, so it
    // stays valid even after the ship that fired it has been destroyed -- which
    // matters, because a missile routinely outlives its launcher.
    const ShipSpec* spec;
    Vec3    pos;
    Vec3    dir;          // unit heading
    float   life;
    float   age;
    int     target;       // enemy index; -1 means the player
    float   last_range;   // for the proximity fuse
    float   lost_at;      // age at which the lock broke, for re-acquisition
    float   trail_acc;
    uint8_t trail_n;
    uint8_t trail_head;
    Vec3    trail[MISSILE_TRAIL];
};

struct Debris {
    bool  alive;
    Vec3  pos;
    Vec3  seg;
    Vec3  vel;
    float life, life0;
};

// A fireball. Debris is the shrapnel; this is the light.
//
// `r` is the radius it opens to, in world units, not the radius right now --
// the drawn size is a curve over its own life, so one number plus an age is the
// whole shape of it. Same life0/life pair the shards use, for the same reason:
// the effect has to know how far through itself it is, not just how long it has
// left.
struct Fireball {
    bool  alive;
    Vec3  pos;
    Vec3  vel;
    float r;
    // How this one goes out, as the exponent on its own brightness decay. Below
    // 1 it collapses early and is gone well before its life is up; above 1 it
    // holds and then drops. Per ball rather than one shared curve, because with a
    // single falloff shape the cluster dimmed as a unit and read as one object
    // fading -- the whole point of spawning several is that they should not agree
    // about when they are finished.
    float fall;
    float life, life0;
};

// Screen flow:
//
//   ATTRACT --tap--> ENTRY --callsign--> SELECT --ship, and the draw is made-->
//                                                                  |
//                                                                  v
//                                       PLAYING <---ready--- BRACKET <-- COURSE
//                                          |                    ^      done/skip
//                                          +---- ROUND_WON -----+
//                                       won a round, not the final
//
// SHIP THEN COURSE. Flying the check-in in an airframe the player has not chosen
// is the wrong way round -- the course is where a class's handling is learned, and
// learning somebody else's is worse than learning nothing. The same course is
// reachable later from the bracket; nothing distinguishes the two runs, because
// after the draw is made there is nothing left to distinguish.
//
// PAUSE is reachable from anything that flies, always on PWR. What the second
// button on it does depends on what it suspended: QUIT from a match, SKIP from
// the course.
enum VgState : uint8_t {
    VG_ATTRACT = 0,
    VG_ENTRY,             // callsign and trail colour
    VG_SELECT,            // ship selection -- once per tournament, then locked
    VG_REPAIR,            // spend credits on hull, reached from the bracket
    VG_BRACKET,           // the tournament map
    VG_INTRO,             // launch cutscene: both fighters introduced
    VG_PLAYING,
    VG_HIT,               // brief invulnerable pause after taking a hit
    VG_KILL,              // opponent is down and talking; player cannot be hurt
    VG_PAUSE,
    VG_COURSE,            // the optional ring course, no stakes
    VG_ROUND_WON,         // beat between winning a match and the bracket redraw
    VG_OVER,              // knocked out -- the run is finished
    VG_WON                // took the whole tournament
};

// --- the set turning on and off --------------------------------------------
//
// Crossing between the menus and the game is a BROADCAST cutting to and from the
// action, so the picture collapses to a scan band and comes back rather than
// simply changing. The effect itself lives in vg_band.cpp; this is the schedule.
//
// The change of state happens at the JOIN, while the screen is black. That is
// the whole reason this is two phases and not one animation: the old scene has
// to be gone before the new one is built, or the wipe closes on one picture and
// opens on the same one.
// The TV transition's types, timing and state moved to vg_states.h -- see the note
// at the top of it. What stayed here is vg_state_go and vg_state_cut, below.

// --- launch cutscene schedule ----------------------------------------------
// Camera adrift, then each fighter flown across the view in turn, with a hard
// cut between them. Skippable, because the fifth time through it is furniture.
// Paced to settle the player rather than to get out of the way. Two seconds of
// empty space first, then four and a half on each fighter -- long enough to
// look at it, which is the point of introducing it at all.
// Each shot is long enough for the gate to open, hold, release the ship, and
// still leave a full fly-by afterwards.
#define INTRO_DRIFT     2.4f
#define INTRO_YOU_END   9.2f
#define INTRO_OPP_START 9.6f
#define INTRO_OPP_END  16.4f
#define INTRO_END      16.8f

// How long the instruments take to come up once the cockpit is back.
#define HUD_BOOT_TIME   1.5f

// How long the panel shows a hit. Long enough to be startling, short enough
// that it is never the reason a second hit lands.
#define DAMAGE_GLITCH   0.85f

// Below this fraction of hull the systems never fully recover: the panel keeps
// a low, permanent fault instead of settling between hits. This is the hull bar
// said a second way -- a number you have to look at, made into something you
// cannot help noticing.
#define DAMAGE_CHRONIC  0.55f

// The entry gate, in four beats. It wipes open upward, holds long enough to be
// registered as a thing in its own right, releases the ship, holds again while
// it clears, then wipes shut the same way.
//
// The register hold is the whole reason this reads as an effect rather than a
// flash: an object that appears and is immediately upstaged never gets looked
// at. The ship is deliberately not there yet.
#define GATE_SWIPE      0.25f
#define GATE_REGISTER   0.50f
#define GATE_CLEAR      0.55f
#define GATE_EMERGE     (GATE_SWIPE + GATE_REGISTER)
#define GATE_TIME       (GATE_SWIPE + GATE_REGISTER + GATE_CLEAR + GATE_SWIPE)
#define GATE_SIZE       210.0f

// After the opponent goes down. Two distinct beats, and they do different jobs.
//
// KILL_SPEECH is how long their last transmission stays up. It is the only line
// in the game the player cannot provoke a second time, so it is given room.
//
// KILL_REFLECT is the silence afterwards -- cockpit, wreckage, nobody talking.
// Cutting from a death straight to a scorecard is what makes a kill feel like a
// score, and the quiet is the only thing that stops it.
//
// The player is invulnerable for both, and free to fly through what is left.
#define KILL_SPEECH     5.6f

// An IFT line holds longer than a pilot's. It is not competing with anything --
// the broadcast only speaks between fights -- and it carries information the
// player cannot get anywhere else, so it is worth reading rather than glancing at.
#define IFT_SPEECH      6.4f

// Shorter over the cutscene, because there the broadcast is not the only thing
// talking. Each fighter's shot runs about 6.8s and already carries a name card;
// the IFT takes the first part of the shot and the card takes the rest, so the
// two SEQUENCE instead of stacking. Overlapped, they read as one garbled caption.
#define IFT_SPEECH_INTRO 3.2f
// One beat of a multi-line announcement. Shorter than a whole statement because
// these are read consecutively -- three at IFT_SPEECH would run twenty seconds
// and the player is sitting in a tube waiting to be allowed to fly.
#define IFT_SPEECH_BEAT  3.0f

// Silence between consecutive lines of one announcement. Without it the queue
// swaps the text on a single frame and three lines read as one paragraph going
// past -- the player is being handed information faster than they can notice a
// new sentence has started, which is what the first playtest of the course
// opening reported. The gap is what makes them three separate things said.
#define IFT_GAP          0.8f
#define KILL_REFLECT    4.6f
#define KILL_BEAT       (KILL_SPEECH + KILL_REFLECT)

// Finishing the ring course gets THE SAME TAIL AS A KILL. Structurally it is the
// same moment: the thing that was in the way is gone, the broadcast says so, and
// there is silence before the game moves you on. Cutting straight from the last
// gate to the tournament map would make the course read as a menu that ended,
// rather than as something the player finished.
//
// KILL_REFLECT exactly, because that is how long an IFT line is left standing
// after a match before the scene changes, and this is meant to land as the same
// beat. There is no loser's transmission to wait out first, so that part has no
// equivalent here.
#define COURSE_DONE_BEAT  KILL_REFLECT

// Victory sequence schedule. The state machine owns it because it decides when
// the state ends; the overlay only draws to it. Each beat finishes before the
// next begins -- overlapping them means neither is ever fully present and the
// moment has nothing to land on.
#define WON_RUMOR_IN    4.2f    // result has cleared; the rumour fades up
#define WON_NAME_IN     8.8f    // rumour has been read; the name takes over
#define WON_RETURN     15.0f    // name has stood alone; back to the menu

// Menu states run the attract autopilot underneath and draw no instruments.
// States that carry no instruments. The cutscene and the death sequence are in
// here for the same reason the menus are: there is no cockpit to report from.
// How many there are. vg_crumb.cpp keeps its own copy of the names and must not
// include this header -- a crash in the game's headers would take the crash
// reporter down with it -- so this number is the contract between the two.
#define VG_STATE_COUNT 14

// WHAT a state is, rather than WHICH state it is.
//
// Every question the rest of the game asks about the current state used to be
// answered by a list of state names written out at the point of asking: one for
// the menus here, one for whether the alerts run, one for whether the engine
// hums, one in the renderer, one in the overlay. Five lists, none of them
// visible from any of the others, all of them needing a new entry whenever a
// state was added -- and nothing to say which one had been forgotten.
//
// The lists are one table now, in vg_game.cpp. Ask what a state IS and the
// answer is the same wherever it is asked from.
// A menu tap: a contact that lifted WITHOUT travelling.
//
// Resolved once by the dispatch rather than in the input layer, because the
// bracket needs one contact to serve as both a pan drag and a button press, and
// only the consumer can know which it turned out to be. Passed to every state's
// update so no state has to work it out again, and so none of them can disagree
// about what a tap is.
struct Tap {
    bool  up;       // it lifted this frame
    float x, y;     // where it went DOWN, not where it came up
};

#define VGS_MENU    0x01u   // a screen, not a cockpit: no HUD, no ship systems
#define VGS_LIVE    0x02u   // the panel answers -- alerts, threat, instruments
#define VGS_ENGINE  0x04u   // the airframe is still under power
#define VGS_COMBAT  0x08u   // a fight is actually in progress
// The backdrop drifts on its own behind this screen, so it is a place and not a
// still. NOT the same set as VGS_MENU, which is the trap: nine states are menus
// and only seven of them drift. INTRO hands the viewpoint to the cutscene camera
// and OVER tumbles the wreck with its own world step, so either one would end up
// running two world motions at once. That was implicit in seven copies of one
// call and is a column now.
#define VGS_DRIFT   0x10u

uint8_t     vg_state_flags(VgState s);

static inline bool vg_state_is_menu(VgState s) {
    return (vg_state_flags(s) & VGS_MENU) != 0u;
}

struct VgGame {
    VgState  state;
    float    state_t;


    // The player's ship, chosen once and flown for a whole tournament.
    ShipClass       ship;
    const ShipSpec* spec;

    // Identity. Three characters like every other entrant, and a hue -- the one
    // thing in the game allowed to carry colour.
    char     callsign[4];
    float    trail_hue;      // 0..1

    // Persistent across runs. NOT reset by starting a tournament: this is the
    // bank, and losing it would defeat the point of a meta-currency.
    int      credits;

    // Set once a tournament has been taken, and never cleared. The rumour in
    // the hangar bays is about YOU from then on -- the intro crawl changes to
    // match, which is the whole point of the name.
    bool     champion;
    // THE NAME, IF IT IS NOT YOURS ANY MORE.
    //
    // Empty until the title changes hands. The legend in the opening crawl has no
    // callsign of its own -- it is a rumour -- but once the player has held the title
    // and lost it, the pilot who took it has a name, and that name is who the next
    // run has to get past. Persisted, so the lineage outlives a power cycle.
    //
    // Three characters and a NUL, like every other callsign. Empty means nobody has
    // inherited it and the legend is the anonymous one from the crawl.
    char     phantom_tag[4];

    float    health;       // absolute hull points remaining
    float    health_max;   // ...out of this, from the chosen class

    float    throttle;    // smoothed by the flight model, 0..1
    // A second, much faster-tracking copy of the same command, used only by the
    // HUD warp. The flight model is deliberately sluggish so committing to speed
    // costs something; the canopy should still flex the moment you move the
    // slider, or the control feels unresponsive even though the ship is behaving
    // exactly as intended.
    float    throttle_vis;
    float    speed;
    float    agility;     // turn-rate multiplier from throttle
    float    bank;        // cosmetic roll, applied at projection only
    // TRUE roll, folded into the world transform, so it composes with pitch and
    // yaw instead of merely rotating the finished image. Accumulated because the
    // backdrop is not carried by that transform and has to be told the total.
    float    roll;
    // This frame's view offset in pixels. The LEVEL behind it lives in
    // vg_shake.cpp, because many things contribute and none of them owns it.
    float    shake_x, shake_y;
    float    hit_flash;
    // Light from an explosion nearby, 0..1, raised by vg_spawn_blast and decayed
    // in vg_world_step. Distinct from hit_flash: that one means YOU were hit and
    // is red, this one means something went off next to you and is not.
    float    blast_flash;

    // The player's weapons. `rounds` rather than `missiles`, because the plural of
    // the thing and the count of the thing being the same word is how you end up
    // reading `vg.missiles` next to `vg.msl[]` and having to stop.
    struct {
        int   rounds;      // remaining in the rack
        float reload_t;
        float fire_gap;
        int   target;      // enemy index, or -1
        float lock_t;      // time held inside the nose cone
        float lock_need;   // time required at the current speed
        bool  locked;
    } wpn;

    MslEvent msl_event;
    float    msl_event_t;  // counts down; the banner shows while positive
    // Every round the player fires must resolve into something the player is
    // told. A single slot dropped outcomes whenever two shots landed close
    // together, so pending ones queue behind the one on screen instead.
    MslEvent msl_queue[6];
    uint8_t  msl_qn;

    // Radio. One line at a time, held by priority so a death cry is never
    // stepped on by the next round going wide.
    struct {
        char        tag[4];
        const char* line;
        float       t;
        uint8_t     pri;
        bool        mark;    // this line opens a run, so it is badged
        float       since;   // seconds since the last transmission began
    } comms;

    // The broadcast voice, on its OWN slot rather than sharing the pilots'.
    //
    // Sharing was the obvious thing and it breaks at the one moment that matters:
    // after a kill the dying pilot holds the comms slot for KILL_SPEECH, which is
    // exactly when the IFT is supposed to be summing up. Two slots let them
    // overlap on purpose, and a loser still talking under the summary is the tone
    // of the whole tournament.
    //
    // No priority field. The IFT never interrupts itself -- it speaks between
    // fights, never during one, so a later line simply replaces an earlier one.
    const char* ift_line;
    float       ift_t;
    // One bit per IftSlot, so each line fires once per match rather than every
    // frame its cue is true.
    uint8_t     ift_fired;

    // What PAUSE suspended. Pause is reachable from more than one state now, so
    // resuming has to put the player back where they were rather than assuming a
    // match -- and the second button on that screen means different things
    // depending on the answer.
    uint8_t     pause_from;
    uint8_t     pause_page;   // 0 the menu, 1 the audio page

    // The mix -- music and sfx levels -- moved to vg_sfx.h as VgVolume.

    // Current roll rate, rad/sec. Carried rather than recomputed, so the airframe
    // has to be spun up and has to be allowed to stop.
    float       roll_rate;

    // The caution annunciators, decided in the update rather than in the draw.
    //
    // They used to be fmodf(state_t, period) inside the HUD, and a modulo of an
    // ever-growing clock by a CHANGING period does not merely change rate -- the
    // phase jumps every time the period moves. The faster the range changed, the
    // more it scrambled, so the blink rate tracked the SHIP'S SPEED instead of
    // the distance, and flying away from a wall beeped exactly as fast as flying
    // into one. A phase that is advanced by dt cannot do that.
    struct {
        float msl_ph,  wall_ph;
        bool  msl_lit, wall_lit;
    } alerts;

    // How hard the airframe is working, 0 at rest and 1 at the reference speed
    // -- and past 1 for a light ship at full. Computed once in the world step
    // and read by both the camera and the panel, so the two cannot drift apart.
    float       buzz;

    // Whether this line is the one that OPENS a run of speech, and so carries
    // the speaker's badge. A continuation does not: see the note on draw_comms()
    // in vg_hud.cpp.
    bool        ift_mark;

    // Looking aft. Held, never toggled: a pilot craning round is doing something
    // continuous and effortful, and a latch would leave a player flying blind
    // forwards without a finger on the panel to remind them why.
    bool        rear_view;

    float       taunt_t;   // countdown to the next unprompted remark

    // The player's ribbon moved to vg_flight.h as PlayerTrail. trail_hue did NOT:
    // it is identity, and RpSave carries it into every recording.

    // A ship nobody is flying: the one shown crossing the view during the
    // launch cutscene, and the wreck the camera tumbles around after a death.
    // Kept apart from the enemy array so neither the AI nor the collision pass
    // can ever see it.
    // Grouped, and the ship inside it is `ship`. It was a bare `cine` next to
    // cine_on, cine_hold and five gate_ fields that all belonged to it, so the
    // group had to take the noun and the Ship had to be named.
    // The launch cutscene's state moved to vg_cine.h as CineState.

    float    hud_boot;     // >0 while the instruments are coming up
    // THE BOOT IS A CHAIN NOW, and these two are the links.
    //
    // A match used to start everything at once: the instruments caught, the cockpit came
    // online and the opponent opened the radio, all inside the same second and a half. The
    // author's word for it was that the three "overlap too tightly", and the fix is to make
    // each one wait for the one before it.
    //
    // So: the view sits dark, the cockpit arrives a region at a time, and only when that is
    // nearly done does `hud_cued` fire and start hud_boot. `ready` follows once the
    // instruments are actually in, and the radio waits for it -- an opponent talking over a
    // panel that is not lit yet is talking to nobody.
    bool     hud_cued;     // the cockpit sequence has called for the instruments
    float    radio_t;      // >0 while the radio is still held shut after SFX_READY
    uint8_t  regions_lit;  // how many cockpit regions have already been beeped for
    bool     ready;        // the radio may open -- BOTH the broadcast and the opponent wait on it
    // >0 while the systems are visibly hurt. Same failure language as the death
    // screen, briefly and at lower severity -- damage and destruction are the
    // same event at different scales, so they read as the same thing happening.
    float    damage_glitch;
    float    cam_zoom;     // 1.0 in flight; only the cutscene moves it

    float    wall_clear;   // distance to the arena boundary, recomputed each frame
    // HOW FAST THE WALL IS ARRIVING, units a second, positive while closing.
    //
    // Distance alone cannot say ''you are about to hit this''. A ship holding station a
    // hundred units off the boundary is safe indefinitely; the same hundred units with the
    // nose pointed at it is a second of life. So the warning reads BOTH: the colour comes
    // from the clearance and the flashing comes from this.
    //
    // The RATE and not the speed, deliberately. Speed would cry wolf every time a fast hull
    // ran parallel to the wall, which is most of a tunnel fight, and it would say nothing
    // about a slow drift straight into it. This is d(clearance)/dt, so it is only large when
    // the clearance is actually being spent.
    //
    // Smoothed: one frame''s difference of two clearances is mostly noise, because the
    // clearance comes out of a torus solve that moves with the whole world transform.
    float    wall_rate;

    // Threat state, recomputed each frame for the HUD. `on` rather than a bare
    // `threat` because the group is the noun now and the flag is one of its
    // fields -- vg.threat.on says which, where vg.threat did not.
    struct {
        bool  on;
        float range;
        Vec3  pos;
    } threat;

    float    spawn_t;

    Ship     enemy[MAX_ENEMIES];
    Missile  msl[MAX_MISSILES];
    Asteroid ast[MAX_ASTEROIDS];
    Debris   deb[MAX_DEBRIS];
    Fireball fire[MAX_FIREBALLS];
    Vec3     star[NUM_STARS];
    uint8_t  star_b[NUM_STARS];
    Vec3     mote[NUM_MOTES];      // near-field dust, for a sense of speed
};

extern VgGame   vg;
extern AstModel vg_models[NUM_MODELS];

// Enemy fighter, in local space (+x right, +y up, +z forward).
//
// Verts 0..4 form a CLOSED hull (nose apex over a four-vertex rear ring), which
// is what hidden-line rendering needs -- the old open skeleton had no faces to
// fill. The tail fin is a flat blade with no volume, so it stays wireframe and
// is drawn over the hull.
#define SHIP_VERTS     7
#define SHIP_FACES     6
#define SHIP_FIN_EDGES 3
extern const Vec3 vg_ship_verts[SHIP_VERTS];
extern uint8_t    vg_ship_faces[SHIP_FACES][3];      // wound outward at init
extern const uint8_t vg_ship_fin[SHIP_FIN_EDGES][2];

// Build the render basis for a ship: columns are right/up/forward, with the
// visual roll folded in.
Mat3 vg_ship_basis(const Ship* s);

void vg_game_init(void);

// Begin a fresh tournament with the chosen ship: full hull, new bracket.
void vg_tournament_begin(ShipClass c);

// Set up the next match against whoever the bracket says. Hull is NOT restored
// here -- damage carries between rounds, and only credits will ever undo it.
void vg_match_start(void);

// --- changing state ---------------------------------------------------------
//
// Three verbs, because there are three different things being done and they
// were previously told apart by which lines the author wrote out at the site.
//
// WHETHER A CHANGE CUTS THROUGH THE SET IS A PROPERTY OF THE EDGE, NOT OF THE
// DESTINATION. It was briefly modelled as a column on the state, which is wrong
// and the code says so plainly: the tournament table is arrived at instantly
// from the repair screen and from the end of a round, and on a cut from the
// course and from the pause menu. Same destination, four callers, two
// behaviours. So the caller says which.
void vg_state_go(VgState to);      // now: enter it, and run its set-up
void vg_state_cut(VgState to);     // through the set, then the same at the join

// Coming back from a suspension, which is NOT an arrival: the state was never
// left. It is not entered again and its set-up does not run -- re-entering
// VG_COURSE here would rebuild the course the player is sitting in.
void vg_state_resume(VgState to);

void vg_game_select_ship(ShipClass c);

// Credits paid out for the most recent round win, for the ROUND WON card.
int  vg_last_purse(void);
void vg_game_update(float dt, const VgInput* in);
