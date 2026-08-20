#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"
#include "vg_input.h"
#include "vg_ship.h"
#include "vg_trailring.h"

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

    // Identity, and the ribbon that carries it. The ring itself is the shared
    // TrailRing -- see vg_trailring.h, where trail.p is explained.
    float   hue;
    ShipTrailRing trail;
};

// MslEvent moved to vg_cockpit.h with the banner it drives.

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
    // The bare ring: no power bytes, because a missile burns at one brightness.
    MissileTrailRing trail;
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
// HOW MUCH LOUDER THE SCREEN ARTIFACT IS ON A HIT, against the ambient damage tearing.
//
// The artifact is what makes a hit LAST. A struck canopy panel flashes white and then sits
// there as static for over a second, and DAMAGE_GLITCH on its own was finished in a blink --
// so the two halves of one event ran on completely different clocks.
#define HIT_GLITCH_AMP  2.6f

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
    // hit_flash and blast_flash moved to vg_cockpit.h as flash.hit and flash.blast. They
    // are what the PANEL does about a hit, not what the hull is; vg_hud_decay already
    // faded them.

    // The missile banner moved to vg_cockpit.h. It is not a broadcast: it is the ship
    // telling its own pilot what happened to a round, which is an instrument reading.

    // The radio moved to vg_comms.h as Broadcast vg_bcast, on channels. It was three
    // blocks of fields here with three sets of rules; adding a fourth voice is now an enum
    // entry and a table row.


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

    // The caution annunciators moved to vg_cockpit.h, inside Cockpit.

    // How hard the airframe is working, 0 at rest and 1 at the reference speed
    // -- and past 1 for a light ship at full. Computed once in the world step
    // and read by both the camera and the panel, so the two cannot drift apart.
    float       buzz;


    // Looking aft. Held, never toggled: a pilot craning round is doing something
    // continuous and effortful, and a latch would leave a player flying blind
    // forwards without a finger on the panel to remind them why.
    bool        rear_view;

    float       taunt_t;   // countdown to the next unprompted remark

    float    cam_zoom;     // 1.0 in flight; only the cutscene moves it

    // The boundary pair moved to vg_flight.h as Wall vg_wall. Only the world step
    // maintains it; the five other writes were the same reseed line copied.

    // The missile threat moved to vg_weapons.h as Threat.

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
