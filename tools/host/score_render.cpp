// Render a score to a WAV file, with the GAME'S OWN synthesiser.
//
// This file has no synthesiser in it. It compiles src/vg/vg_synth.cpp, the same
// file the firmware runs, and drives it. tools/README.md says there must not be
// a second copy of the synthesiser on this computer. There is not one. There is
// one copy, built twice.
//
// Build and run through tools/score_audio.py. That script finds the compiler.
//
//     score_render.exe JOB.txt OUT.WAV
//
// The job file holds one line for each setting, then one line for each note:
//
//     rate 22050
//     mix 0.49
//     len_ms 16000
//     tail_ms 900
//     speaker 1
//     loop 1
//     note <t_ms> <wave> <f0> <f1> <life> <atk> <sus> <gain> <lp> <delay> <mod> <depth>
//
// `wave` is 0 for a square, 1 for noise and 2 for a sine, which is SynthWave.
//
// `loop 1` fades the last few milliseconds to zero. A score that still sounds at
// its last sample would click every time the loop turned over.
//
// WARNING: `speaker 1` adds a filter that is an APPROXIMATION of the device
// driver. Nobody measured the driver to make it. With `speaker 0` the samples
// are what the firmware synthesiser gives, and those are exact.
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vg/vg_port.h"    // VG_AUDIO_RATE
#include "../../src/vg/vg_prof.h"    // g_synth_peak
#include "../../src/vg/vg_synth.h"
#include "speaker.h"

struct Job {
    int   t_ms;
    SynthLayer layer;
};

static Job  s_job[8192];
static int  s_jobs = 0;

static void put32(FILE* f, unsigned v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, unsigned short v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char* path, const short* pcm, int n, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const unsigned bytes = (unsigned)n * 2u;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                       // PCM
    put16(f, 1);                       // mono
    put32(f, (unsigned)rate);
    put32(f, (unsigned)rate * 2u);     // bytes for each second
    put16(f, 2);                       // bytes for each frame
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, bytes);
    fwrite(pcm, 2, (size_t)n, f);
    fclose(f);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "give a job file and an output file\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    int   rate = 22050, len_ms = 4000, tail_ms = 900, speaker = 0, loop = 0;
    float mix = 0.49f, corner = 300.0f;
    char  line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "rate ", 5))          rate = atoi(line + 5);
        else if (!strncmp(line, "len_ms ", 7))   len_ms = atoi(line + 7);
        else if (!strncmp(line, "tail_ms ", 8))  tail_ms = atoi(line + 8);
        else if (!strncmp(line, "speaker ", 8))  speaker = atoi(line + 8);
        else if (!strncmp(line, "loop ", 5))     loop = atoi(line + 5);
        else if (!strncmp(line, "mix ", 4))      mix = (float)atof(line + 4);
        else if (!strncmp(line, "corner ", 7))   corner = (float)atof(line + 7);
        else if (!strncmp(line, "note ", 5)) {
            if (s_jobs >= (int)(sizeof s_job / sizeof s_job[0])) continue;
            Job& j = s_job[s_jobs];
            int wave = 0;
            const int got = sscanf(line + 5, "%d %d %f %f %f %f %f %f %f %f %f %f",
                                   &j.t_ms, &wave, &j.layer.f0, &j.layer.f1,
                                   &j.layer.life, &j.layer.attack, &j.layer.sustain,
                                   &j.layer.gain, &j.layer.lp_hz, &j.layer.delay,
                                   &j.layer.mod_hz, &j.layer.mod_depth);
            if (got != 12) {
                fprintf(stderr, "bad note line: %s", line);
                fclose(f);
                return 2;
            }
            j.layer.wave = (SynthWave)wave;
            s_jobs++;
        }
    }
    fclose(f);

    if (rate != VG_AUDIO_RATE) {
        // The synthesiser is built against VG_AUDIO_RATE. It cannot run at another
        // rate, so stop rather than write a file that plays at the wrong speed.
        fprintf(stderr, "rate %d does not match VG_AUDIO_RATE %d\n", rate, VG_AUDIO_RATE);
        return 2;
    }

    const int total = (int)((long long)(len_ms + tail_ms) * rate / 1000);
    short* pcm = (short*)calloc((size_t)total, sizeof(short));
    if (!pcm) {
        fprintf(stderr, "out of memory for %d samples\n", total);
        return 2;
    }

    vg_synth_reset();
    Speaker sp;
    sp.set(corner, (float)rate);

    const int BLOCK = 32;
    int next = 0;
    for (int at = 0; at < total; at += BLOCK) {
        const int n = (total - at < BLOCK) ? total - at : BLOCK;
        const int now_ms = (int)((long long)at * 1000 / rate);
        while (next < s_jobs && s_job[next].t_ms <= now_ms) {
            vg_synth_layer(&s_job[next].layer, 1.0f);
            next++;
        }
        vg_synth_render(pcm + at, n, mix);
    }

    if (speaker) sp.block(pcm, total);

    if (loop) {
        // Three milliseconds, which is under a tenth of the shortest cue here and
        // is not audible as a fade. It only removes the step at the loop point.
        int fade = rate * 3 / 1000;
        if (fade > total) fade = total;
        for (int i = 0; i < fade; i++) {
            const int at = total - fade + i;
            pcm[at] = (short)((float)pcm[at] * (float)(fade - 1 - i) / (float)fade);
        }
    }

    const int ok = write_wav(argv[2], pcm, total, rate);
    free(pcm);
    if (!ok) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    printf("%d samples, %d notes, peak %.3f\n", total, s_jobs, (double)g_synth_peak);
    return 0;
}
