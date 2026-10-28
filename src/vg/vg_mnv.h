#pragma once
// THE MANOEUVRES, and the interface a chooser calls. The proposal is
// captures/brk/MANOEUVRES_v7.md; the names, units and formats the AI tab's tools read
// are pinned in captures/ai_eval/selector/STAGE1_INTERFACE.md.
//
// An opponent's frame is the floors -- the wall, the lookahead, the merge floor -- and,
// if none of them flew, a manoeuvre picked by a chooser. Today the one chooser is THE
// RULES: the old priority chain, asked in its old order, so the fights are the fights
// the chain flew.
#include <stdint.h>

// THE ONE LIST, APPEND-ONLY. An id is never reused or renumbered: the game, the
// trainer and a selector's weights all read it. "none" is never valid and never
// chosen; it is what a ship is running before its first manoeuvre of the bout.
#define VG_MNV_LIST(X)                                                          \
    X(NONE,     "none")      /* nothing has flown since the spawn */            \
    X(HUNT,     "hunt")      /* pursuit, and the track inside lock range */     \
    X(BREAKOUT, "breakout")  /* the hunt's break-away leg: AGG_IN_MIN */        \
    X(CBREAK,   "cbreak")    /* out of a circle: extend, turn in, run */        \
    X(RESET,    "reset")     /* the stale reset: ENEMY_STALE_TIME */            \
    X(EVADE,    "evade")     /* a round coming in; the network's BREAK mode */  \
    X(TAIL,     "tail")      /* somebody on its six */                          \
    X(EXPOSED,  "exposed")   /* the exposed shot: ENEMY_EXPO_MAX */             \
    X(LIGHT,    "light")     /* BALLISTA lights its round */                    \
    X(STANDOFF, "standoff")  /* BALLISTA's class plan; the HOLD mode */         \
    X(DRY,      "dry")       /* an empty rack; the EXTEND mode */               \
    X(PRESS,    "press")     /* the attack pass, the slow-player press, v1 */   \
    X(PLAN,     "plan")      /* the class plan of AEGIS, LANCE and CHARIOT */   \
    X(NET,      "net")       /* the network flies it */

enum VgMnv : uint8_t {
#define VG_MNV_ENUM(id, name) MNV_##id,
    VG_MNV_LIST(VG_MNV_ENUM)
#undef VG_MNV_ENUM
    VG_MNV_N
};
extern const char* const VG_MNV_NAME[VG_MNV_N];

// HOW A MANOEUVRE ENDED. done: it reached its goal. aborted: what it needed went away.
// max: its ceiling. switch: the chooser took another one while this one still could
// have flown. bout: the bout or the ship ended.
enum VgMnvStatus : uint8_t { MNV_RUNNING, MNV_DONE, MNV_ABORTED, MNV_MAX, MNV_SWITCH, MNV_BOUT };
extern const char* const VG_MNV_STATUS_NAME[];

// THE FLOORS, which are not manoeuvres: they take the frame from whatever is running,
// and the running one's clock keeps going. SAFETY is the selector's own (not built).
enum VgFloor : uint8_t { FLOOR_NONE, FLOOR_WALL, FLOOR_AVOID, FLOOR_MERGE, FLOOR_SAFETY };
extern const char* const VG_FLOOR_NAME[];

// WHO CHOSE. The rules, a trained selector, or --force-manoeuvre.
enum { VG_MNV_BY_RULES, VG_MNV_BY_SELECTOR, VG_MNV_BY_FORCE };
extern const char* const VG_MNV_BY_NAME[];

// THE ACCESSORS (STAGE1_INTERFACE.md section 2). Valid after the chain has run for
// opponent i this frame. All of them read; none of them changes anything.
VgMnv       vg_mnv_rules_choice(int i);   // a floor frame: the first valid in the rules' order
uint32_t    vg_mnv_valid_mask(int i);     // bit m: m may fly now. 0 unless vg_mnv_want_mask
VgMnv       vg_mnv_current(int i);
float       vg_mnv_time_in(int i);        // s since the current one started
void        vg_mnv_params(int i, float p[3]);
VgFloor     vg_mnv_preempted(int i);      // the floor that flew this frame
VgMnvStatus vg_mnv_last_exit(int i);
float       vg_mnv_min_t(int m, int cls); // MNV_TIME in cfg_combat.h, s
float       vg_mnv_max_t(int m, int cls);

// WORK OUT THE VALID MASK EVERY FRAME. Off unless a harness turns it on: the device
// never needs it. It changes no fight -- the identity gate runs with it on.
extern bool vg_mnv_want_mask;

// THE CHOICE LOG. One event per manoeuvre start and end and per floor span, never one
// per frame. The desktop prints them (--log-manoeuvre); nothing in the game reads them.
enum { VG_MNV_EV_START, VG_MNV_EV_END, VG_MNV_EV_FLOOR_ON, VG_MNV_EV_FLOOR_OFF };
struct VgMnvEvent {
    uint8_t kind;      // VG_MNV_EV_*
    uint8_t index;     // the opponent
    uint8_t cls;       // its hull class, 255 if none
    uint8_t mnv;       // VgMnv: the one starting or ending, or running under a floor
    uint8_t status;    // VgMnvStatus: why it ended
    uint8_t floor;     // VgFloor
    uint8_t by;        // VG_MNV_BY_*
    float   p[3];      // its parameters, at the start
    float   t;         // s: how long it ran, or how long the floor held
};
extern void (*vg_mnv_tap)(const VgMnvEvent* e);

// THE HOST'S FRAME COUNTER, for the log: the frames flown before this one, the same
// count as the desktop's "frames N" line. The desktop sets it; on the device it stays 0.
extern uint32_t vg_host_frame;

// THE BOUT IS OVER FOR THIS SHIP: ends its manoeuvre and any floor span, for the log.
struct Ship;
void vg_mnv_bout_end(Ship* s, int index);
