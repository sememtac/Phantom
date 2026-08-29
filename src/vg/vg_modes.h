#pragma once
// WHAT AN NPC CAN BE DOING, as opposed to what it is doing this frame.
//
// A stick position is a reflex. A MODE is an intention held for seconds, and the
// difference is the whole reason this file exists: measured over every recording,
// a mode lasts a median of 2.08 seconds, which is a plan, while the stick it
// implies changes sixty times in that span.
//
// THIS LIST IS THE CONTRACT, and it is deliberately the only copy. The trainer
// parses it out of this file to know what to label and in what order; the game
// dispatches on it; the generated weights record how many there were when they
// were fitted. Three things that must agree, agreeing because they read one list.
//
// TO ADD A MODE, three edits, and nothing else:
//   1. a row here, AT THE END -- the order is the order of the network's outputs,
//      so inserting in the middle silently relabels every weight ever trained
//   2. a labeller in tools/train_pilot.py, which decides from a RECORDING which
//      frames were that mode. It may look at the future; the network may not.
//   3. a case in vg_ai.cpp that flies it. A mode with no case falls back to the
//      class tactic, which is wrong but safe.
//
// A network trained before a mode was added keeps working: the count is recorded
// in its header and checked, and a mismatch declines rather than mapping old
// weights onto a new vocabulary.
#define VG_MODE_LIST(X)                                                        \
    /* id      what the pilot is doing                                      */ \
    X(PRESS,  "PRESS")   /* close it down and take the shot                 */ \
    X(HOLD,   "HOLD")    /* keep this range and this angle; work the lock   */ \
    X(BREAK,  "BREAK")   /* something is tracking me and I am answering it  */ \
    X(EXTEND, "EXTEND")  /* open the range deliberately, reset the geometry */

enum VgMode {
#define X(id, name) VG_MODE_##id,
    VG_MODE_LIST(X)
#undef X
    VG_MODE_N
};

// The names, for the panel readout and for anything that has to print one.
static const char* const VG_MODE_NAME[] = {
#define X(id, name) name,
    VG_MODE_LIST(X)
#undef X
};
