#pragma once
#include "vg_score.h"

// ===========================================================================
// WHAT MUSIC THE GAME HAS, AND WHERE EACH PIECE PLAYS
//
// A table, for the same reason vg_sfx.cpp is one: adding a theme should be
// adding a row and a baked header, not editing a screen. The screens used to
// name a score directly, which worked for exactly one score.
//
// One tune to a row. The score itself is a file in design/score/ and a header
// baked from it by tools/score.py; nothing here holds notes.
//
// WHICH TUNE PLAYS IS A FUNCTION OF THE STATE, worked out once a frame rather
// than started and stopped by hand at every way in and out. The title screen
// alone had three ways in and only one of them ran the entry hook, which is why
// the theme did not start at boot; a rule that is read every frame cannot have
// that fault.
// ===========================================================================

enum VgTune : unsigned char {
    TUNE_TITLE = 0,     // the attract screen
    TUNE_SEMI,          // the semi final, round 2
    TUNE_COUNT          // also means "nothing should be playing"
};

struct VgTuneDef {
    const char*        name;    // for the telemetry, and for reading the table
    const VgScoreStep* steps;
    int                n_steps;
    const SynthLayer*  notes;
    int                len_ms;
};

// The tune a round of the tournament gets, or TUNE_COUNT for a round with none.
// Rounds are 0..3: last sixteen, quarter final, semi final, final.
VgTune vg_music_for_round(unsigned char round);

// Start one, looping. Starting the tune already playing does nothing, so this
// may be called every frame.
void vg_music_play(VgTune t, bool loop);

// Work out what should be playing from the state of the game, and make it so.
// Call it once a frame, before vg_score_update.
void vg_music_update(void);

// What is playing, or TUNE_COUNT. For the telemetry.
VgTune vg_music_now(void);
const char* vg_music_name(VgTune t);
