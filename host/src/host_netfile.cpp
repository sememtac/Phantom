// Loading an int8 pilot network from a file, for the training harness.
//
// DESKTOP ONLY. The board flies the network compiled into it; this exists so that
// training by evolution can fly a new candidate in every run without rebuilding the
// game. The file is written by tools/quantize_pilot.py --blob, which is also the
// reference for the arithmetic -- see VgNetQ in vg_net.h.
//
// THE FORMAT, little-endian, in this order. Every field is checked, and a file that
// does not fit the observation or the limits is refused rather than flown: a
// network read with the wrong layout flies happily into the ground.
//
//   "PHNQ", int32 version (1)
//   int32 n_in, nh, n_l, n_out, n_mode, in_fix, in_shift, shift, lut_half,
//         m_half, fire_acc, n_ships, ship_n, has_fire_ship
//   float ship_tol
//   int32 in_mean[n_in], in_mul[n_in]
//   for each layer l = 0 .. n_l: int8 w[rows*cols], int32 b[rows], int32 m[rows]
//         (rows = nh for a hidden layer and n_out for the last; cols = n_in for
//          the first and nh after it)
//   int32 m_mode[n_mode]
//   int8  lut[2*lut_half + 1]
//   float ships[n_ships * ship_n]
//   int32 fire_acc_ship[n_ships]          only if has_fire_ship
//   "END!"

#include "vg_net.h"
#include "vg_bot.h"     // VG_OBS_N
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

template <typename T>
static T* read_array(FILE* f, size_t n, bool* ok) {
    if (!*ok) return nullptr;
    if (n == 0) return nullptr;
    T* p = (T*)malloc(n * sizeof(T));
    if (!p || fread(p, sizeof(T), n, f) != n) { *ok = false; free(p); return nullptr; }
    return p;
}

static bool read_i32(FILE* f, int32_t* v) { return fread(v, 4, 1, f) == 1; }

const VgNetQ* host_net_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "net-file: cannot open %s\n", path); return nullptr; }
    char magic[4];
    int32_t version = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PHNQ", 4) != 0 ||
        !read_i32(f, &version) || version != 1) {
        fprintf(stderr, "net-file: %s is not a version 1 network file\n", path);
        fclose(f);
        return nullptr;
    }
    int32_t h[14];
    for (int i = 0; i < 14; i++) {
        if (!read_i32(f, &h[i])) { fclose(f); fprintf(stderr, "net-file: short header\n"); return nullptr; }
    }
    VgNetQ* q = (VgNetQ*)calloc(1, sizeof(VgNetQ));
    if (!q) { fclose(f); fprintf(stderr, "net-file: out of memory\n"); return nullptr; }
    q->n_in = h[0]; q->nh = h[1]; q->n_l = h[2]; q->n_out = h[3]; q->n_mode = h[4];
    q->in_fix = h[5]; q->in_shift = h[6]; q->shift = h[7]; q->lut_half = h[8];
    q->m_half = h[9]; q->fire_acc = h[10]; q->n_ships = h[11]; q->ship_n = h[12];
    const bool has_fire_ship = h[13] != 0;

    // The limits that keep a bad file from running off the end of a stack array.
    if (q->n_in != VG_OBS_N || q->nh < 1 || q->nh > VG_NET_Q_MAX || q->n_l < 1 ||
        q->n_l > 3 || q->n_out < 4 || q->n_out > VG_NET_Q_MAX || q->n_mode < 0 ||
        4 + q->n_mode > q->n_out || q->shift < 1 || q->shift > 40 || q->in_shift < 1 ||
        q->in_shift > 40 || q->lut_half < 1 || q->lut_half > 4096 || q->n_ships < 0 ||
        // THE INPUT SCALE. gq_run casts obs * in_fix to int32, and the observation
        // is built to stay inside -1..1 (1.000 is the largest value in every M1 and
        // a_ recording). Up to 2^24 keeps the cast in range for |obs| up to 128.
        q->in_fix < 1 || q->in_fix > (1 << 24) ||
        q->n_ships > 16 || (q->n_ships > 0 && (q->ship_n < 1 || q->ship_n > q->n_in))) {
        fprintf(stderr, "net-file: %s has sizes this game cannot fly (n_in %d, observation %d)\n",
                path, q->n_in, VG_OBS_N);
        fclose(f);
        free(q);
        return nullptr;
    }
    bool ok = fread(&q->ship_tol, 4, 1, f) == 1;
    q->in_mean = read_array<int32_t>(f, (size_t)q->n_in, &ok);
    q->in_mul  = read_array<int32_t>(f, (size_t)q->n_in, &ok);
    for (int l = 0; l <= q->n_l; l++) {
        const int rows = (l < q->n_l) ? q->nh : q->n_out;
        const int cols = (l == 0) ? q->n_in : q->nh;
        q->w[l] = read_array<int8_t>(f, (size_t)rows * cols, &ok);
        q->b[l] = read_array<int32_t>(f, (size_t)rows, &ok);
        q->m[l] = read_array<int32_t>(f, (size_t)rows, &ok);
    }
    // THE BIASES START EACH int32 SUM, so a huge one from a file could overflow it.
    // A sum of VG_NET_Q_MAX products of two int8 values is under 2^21; a bias under
    // 2^24 leaves the total far inside int32. Ours are in the tens of thousands.
    for (int l = 0; ok && l <= q->n_l; l++) {
        const int rows = (l < q->n_l) ? q->nh : q->n_out;
        for (int j = 0; j < rows; j++)
            if (q->b[l][j] > (1 << 24) || q->b[l][j] < -(1 << 24)) ok = false;
    }
    q->m_mode = read_array<int32_t>(f, (size_t)q->n_mode, &ok);
    q->lut    = read_array<int8_t>(f, (size_t)(2 * q->lut_half + 1), &ok);
    q->ships  = read_array<float>(f, (size_t)q->n_ships * q->ship_n, &ok);
    if (has_fire_ship) q->fire_acc_ship = read_array<int32_t>(f, (size_t)q->n_ships, &ok);
    char end[4];
    if (ok && (fread(end, 1, 4, f) != 4 || memcmp(end, "END!", 4) != 0)) ok = false;
    fclose(f);
    if (!ok) {
        fprintf(stderr, "net-file: %s is cut short, has extra data, or has a bias out of range\n",
                path);
        return nullptr;     // the arrays leak: this happens once, at start-up
    }
    printf("net-file: %s, %d-%dx%d-%d, %d ships\n", path, q->n_in, q->nh, q->n_l,
           q->n_out, q->n_ships);
    return q;
}
