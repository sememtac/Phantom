// Play a score LIVE, with the game's own synthesiser.
//
// This file has no synthesiser in it. It compiles src/vg/vg_synth.cpp, the same
// file the firmware runs, and drives it in real time. score_render.cpp does the
// same job to a file. tools/README.md says why there is only one copy.
//
// tools/score_live.py starts this and talks to it. It reads a command on each
// line of standard input and never blocks the sound to do it.
//
//     load <count>   then <count> lines, each one an event:
//                    <t_ms> <part> <wave> <f0> <f1> <life> <atk> <sus> <gain>
//                    <lp> <mod> <depth>
//     len <ms>       the length of the loop
//     loop <0|1>
//     mute <part> <0|1>
//     play | stop | seek <ms>
//                    `stop` keeps the position, so it is a pause. To rewind,
//                    send `seek 0` as well.
//     mix <0..1>
//     speaker <0|1>  add the filter that approximates the device driver
//     hit <wave> <f0> <f1> <life> <atk> <sus> <gain> <lp> <mod> <depth>
//                    sound one note NOW, whether or not the score plays. This is
//                    what makes a note audible the moment somebody clicks it.
//     quit
//
// It prints `pos <ms> <playing>` about ten times a second, so the window can draw
// a playhead that stays with the sound, and can tell when the end was reached.
//
// WARNING: the music has 8 voices of its own in the synthesiser. A preview note
// takes one of them, the same as any other note. That is true on the device too,
// so what you hear here is what the device would do.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/vg/vg_port.h"    // VG_AUDIO_RATE
#include "../../src/vg/vg_prof.h"
#include "../../src/vg/vg_synth.h"
#include "speaker.h"

#define NBUF 6                       // buffers in the ring
#define BUFN 256                     // samples in one buffer, about 11.6 ms

struct Ev {
    long long  at;                   // samples from the start of the score
    int        part;
    SynthLayer layer;
};

static std::mutex        s_lock;
static std::vector<Ev>   s_ev;
static bool              s_mute[128];
static long long         s_len = 0;  // samples in the loop
static long long         s_pos = 0;
static size_t            s_next = 0; // the next event at or after s_pos
static bool              s_play = false;
static bool              s_loop = true;
static float             s_mix = 0.49f;
// ATOMIC, and it must stay that way. As a plain bool the compiler is free to
// hoist the load out of the audio loop, and then the thread never sees the stop.
// The program then never exits, and it holds the sound card and its own file, so
// the next build cannot write score_live.exe.
static std::atomic<bool> s_run(true);
static std::vector<SynthLayer> s_hits;   // preview notes waiting to sound
static bool              s_speaker = false;
static Speaker           s_sp;

// Put s_next back in step with s_pos. Call it after a seek or a wrap.
static void reseek(void) {
    s_next = 0;
    while (s_next < s_ev.size() && s_ev[s_next].at < s_pos) s_next++;
}

// Fill one buffer. Events sound at their exact sample, not at the start of the
// buffer, so a drum lands where it was written.
static void fill(short* out, int n) {
    std::lock_guard<std::mutex> hold(s_lock);

    for (size_t i = 0; i < s_hits.size(); i++) vg_synth_note(&s_hits[i], 1.0f);
    s_hits.clear();

    if (!s_play || s_len <= 0) {
        // Still render. A preview note must sound while the score is stopped.
        vg_synth_render(out, n, s_mix);
        if (s_speaker) s_sp.block(out, n);
        return;
    }

    int done = 0;
    while (done < n) {
        const long long next_ev = (s_next < s_ev.size()) ? s_ev[s_next].at : (1LL << 62);
        const long long target  = (next_ev < s_len) ? next_ev : s_len;
        int chunk = (int)(target - s_pos);
        if (chunk > n - done) chunk = n - done;

        if (chunk > 0) {
            vg_synth_render(out + done, chunk, s_mix);
            s_pos += chunk;
            done  += chunk;
            continue;
        }
        if (s_pos >= s_len) {
            if (!s_loop) {
                s_play = false;
                vg_synth_render(out + done, n - done, s_mix);
                if (s_speaker) s_sp.block(out, n);
                return;
            }
            s_pos = 0;
            reseek();
            continue;
        }
        // An event falls on this sample. Sound it unless its part is off.
        const Ev& e = s_ev[s_next];
        if (e.part < 0 || e.part >= 128 || !s_mute[e.part]) {
            vg_synth_note(&e.layer, 1.0f);
        }
        s_next++;
    }
    if (s_speaker) s_sp.block(out, n);
}

static void audio_thread(void) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = VG_AUDIO_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 2;
    wf.nAvgBytesPerSec = VG_AUDIO_RATE * 2;

    HANDLE ready = CreateEvent(NULL, FALSE, FALSE, NULL);
    HWAVEOUT hwo = NULL;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, (DWORD_PTR)ready, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "cannot open the sound card\n");
        fflush(stderr);
        return;
    }

    static short   buf[NBUF][BUFN];
    static WAVEHDR hdr[NBUF];
    memset(hdr, 0, sizeof hdr);
    for (int i = 0; i < NBUF; i++) {
        fill(buf[i], BUFN);
        hdr[i].lpData         = (LPSTR)buf[i];
        hdr[i].dwBufferLength = BUFN * 2;
        waveOutPrepareHeader(hwo, &hdr[i], sizeof hdr[i]);
        waveOutWrite(hwo, &hdr[i], sizeof hdr[i]);
    }

    int since = 0;
    while (s_run) {
        WaitForSingleObject(ready, 100);
        for (int i = 0; i < NBUF; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            waveOutUnprepareHeader(hwo, &hdr[i], sizeof hdr[i]);
            fill(buf[i], BUFN);
            hdr[i].dwFlags        = 0;
            hdr[i].dwBufferLength = BUFN * 2;
            waveOutPrepareHeader(hwo, &hdr[i], sizeof hdr[i]);
            waveOutWrite(hwo, &hdr[i], sizeof hdr[i]);
            if (++since >= 8) {
                since = 0;
                // ALWAYS the real position, playing or not. A paused playhead
                // has to stay where it is, and a seek has to be seen at once.
                long long ms;
                int going;
                {
                    std::lock_guard<std::mutex> hold(s_lock);
                    ms = s_pos * 1000 / VG_AUDIO_RATE;
                    going = s_play ? 1 : 0;
                }
                printf("pos %lld %d\n", ms, going);
                fflush(stdout);
            }
        }
    }
    waveOutReset(hwo);
    for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(hwo, &hdr[i], sizeof hdr[i]);
    waveOutClose(hwo);
    CloseHandle(ready);
}

static int read_layer(const char* s, SynthLayer* l) {
    int wave = 0;
    const int got = sscanf(s, "%d %f %f %f %f %f %f %f %f %f",
                           &wave, &l->f0, &l->f1, &l->life, &l->attack,
                           &l->sustain, &l->gain, &l->lp_hz, &l->mod_hz,
                           &l->mod_depth);
    l->wave  = (SynthWave)wave;
    l->delay = 0.0f;
    return got == 10;
}

int main(void) {
    memset(s_mute, 0, sizeof s_mute);
    vg_synth_reset();
    std::thread audio(audio_thread);

    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "quit", 4)) break;

        if (!strncmp(line, "load ", 5)) {
            const int count = atoi(line + 5);
            std::vector<Ev> next;
            next.reserve(count > 0 ? count : 1);
            for (int i = 0; i < count; i++) {
                if (!fgets(line, sizeof line, stdin)) break;
                Ev e;
                int t_ms = 0;
                char* rest = line;
                t_ms   = (int)strtol(rest, &rest, 10);
                e.part = (int)strtol(rest, &rest, 10);
                if (!read_layer(rest, &e.layer)) continue;
                e.at = (long long)t_ms * VG_AUDIO_RATE / 1000;
                next.push_back(e);
            }
            std::lock_guard<std::mutex> hold(s_lock);
            s_ev.swap(next);
            reseek();
            printf("loaded %d\n", (int)s_ev.size());
            fflush(stdout);
        } else if (!strncmp(line, "len ", 4)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_len = (long long)atoi(line + 4) * VG_AUDIO_RATE / 1000;
        } else if (!strncmp(line, "loop ", 5)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_loop = atoi(line + 5) != 0;
        } else if (!strncmp(line, "mute ", 5)) {
            int part = 0, off = 0;
            if (sscanf(line + 5, "%d %d", &part, &off) == 2 && part >= 0 && part < 128) {
                std::lock_guard<std::mutex> hold(s_lock);
                s_mute[part] = off != 0;      // takes effect on the next buffer
            }
        } else if (!strncmp(line, "play", 4)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_play = true;
        } else if (!strncmp(line, "stop", 4)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_play = false;
            vg_synth_silence();
        } else if (!strncmp(line, "seek ", 5)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_pos = (long long)atoi(line + 5) * VG_AUDIO_RATE / 1000;
            reseek();
        } else if (!strncmp(line, "speaker ", 8)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_speaker = atoi(line + 8) != 0;
            s_sp.set(300.0f, (float)VG_AUDIO_RATE);
        } else if (!strncmp(line, "mix ", 4)) {
            std::lock_guard<std::mutex> hold(s_lock);
            s_mix = (float)atof(line + 4);
        } else if (!strncmp(line, "hit ", 4)) {
            SynthLayer l;
            if (read_layer(line + 4, &l)) {
                std::lock_guard<std::mutex> hold(s_lock);
                s_hits.push_back(l);          // sounds on the next buffer
            }
        }
    }

    s_run = false;
    audio.join();
    return 0;
}
