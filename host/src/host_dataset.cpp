// Writing (observation, action) pairs to a file, for training something to fly.
//
// DESKTOP ONLY, and that is why it lives here rather than in src/vg. The board
// has no use for a dataset and no filesystem to put one on; the game exposes a
// null function pointer and this is what points it somewhere.
//
// THE FORMAT IS DELIBERATELY DUMB: a short header, then fixed-size records of
// float32, little-endian, no framing and no compression. It is read by numpy in
// two lines and it cannot drift out of step with the game, because the header
// carries the two numbers that would let it -- the observation width and the
// action width. A reader that finds numbers it does not expect should stop
// rather than silently interpret an old layout.

#include "vg_bot.h"
#include "vg_input.h"
#include <stdio.h>
#include <string.h>

static FILE*    s_f = nullptr;
static uint32_t s_rows = 0;

// HOW MANY NUMBERS AN ACTION IS. The three axes the seat can move plus the
// trigger. Roll is deliberately absent: it is a held button and a modifier on
// yaw rather than an axis of its own, so a policy that emitted it would be
// emitting something the control scheme cannot take.
#define ACT_N 4

// 'PHOB', a version, and then the two widths. Anything reading this must check
// all four -- see the note above.
static const char MAGIC[4] = { 'P', 'H', 'O', 'B' };
#define DATASET_VERSION 1

bool host_dataset_open(const char* path);
void host_dataset_close(void);

static void tap(const VgObs* o, const VgInput* in) {
    if (!s_f || !o || !in) return;

    // Skipped when there is nothing to fight. Those frames are the ship flying
    // level in an empty sky, and a dataset full of them teaches a policy that
    // the right answer is usually to do nothing -- which is true of the frames
    // and useless as a pilot.
    if (!o->has_target) return;

    float act[ACT_N];
    act[0] = in->pitch;
    act[1] = in->yaw;
    act[2] = in->throttle;
    act[3] = in->fire_edge ? 1.0f : 0.0f;

    fwrite(o->v, sizeof(float), VG_OBS_N, s_f);
    fwrite(act,  sizeof(float), ACT_N,    s_f);
    s_rows++;
}

bool host_dataset_open(const char* path) {
    if (s_f) return true;
    s_f = fopen(path, "wb");
    if (!s_f) return false;

    uint32_t hdr[3] = { 0, (uint32_t)VG_OBS_N, (uint32_t)ACT_N };
    memcpy(&hdr[0], MAGIC, 4);
    // The version rides in the top half of the observation width, which would be
    // a nasty trick if the widths were ever going to be large. They are not, and
    // it keeps the header to three words that numpy reads as one call.
    hdr[1] |= (uint32_t)DATASET_VERSION << 16;
    fwrite(hdr, sizeof(uint32_t), 3, s_f);

    s_rows = 0;
    vg_bot_tap = tap;
    return true;
}

void host_dataset_close(void) {
    if (!s_f) return;
    vg_bot_tap = nullptr;
    fclose(s_f);
    s_f = nullptr;
    printf("dataset: %u rows of %d obs + %d act\n",
           (unsigned)s_rows, VG_OBS_N, ACT_N);
}
