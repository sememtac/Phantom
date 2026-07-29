#pragma once
#include <stdint.h>

// ===========================================================================
// DISPLAY FAILURE
//
// One vocabulary for every state where the readout is in trouble, so that
// trouble always looks like the same kind of trouble. Taking a hit, being
// destroyed, and holding the throttle against the stop are the same failure at
// three severities -- the ship's systems are being damaged, overwhelmed, or
// strained -- and they read as one idea because they are drawn by one piece of
// code rather than three that happen to resemble each other.
//
// Everything here is hashed off a time bucket rather than sampled per frame.
// Two consequences, both wanted: the effect runs at its own rate instead of
// strobing at whatever the frame rate happens to be, and it holds still between
// buckets, which is what separates damage from noise. Noise has no location, so
// there is nothing wrong with it -- it is simply everywhere.
// ===========================================================================

uint32_t vg_glitch_hash(uint32_t x);

// Deterministic panel displacement. `rate` is the bucket rate in Hz, so callers
// that want to disagree with each other simply pass different ones.
void vg_glitch_offset(float t, float rate, float amp, float* dx, float* dy);

// Patches of panel that stick, die and re-form. `sev` 0..1 scales how many,
// how large and how often they appear.
//
// Two clocks inside: a slow one places the patches so each persists long enough
// to be seen as damage somewhere specific, and a fast one flashes and nudges
// them so the fault stays put while the picture in it does not.
void vg_glitch_patches(float t, float sev);

// Horizontal tears, displaced sideways as well as drawn -- a tear is shifted
// signal, not a stripe laid over the top.
void vg_glitch_tears(float t, float sev);

// True on the rare bucket where the whole frame should be dropped. A display
// that stutters is broken in a way one that merely flickers is not.
bool vg_glitch_dropout(float t, float sev);
