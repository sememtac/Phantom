// The desktop entry point.
//
// On the device the Arduino core calls setup() once and loop() for ever. That is
// all this does, so main.cpp -- the whole frame sequence, the telemetry, the
// crumbs -- is the same file running here as on the board.
#include "host_window.h"
#include "host_opts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined in src/main.cpp, compiled unchanged.
extern void setup(void);
extern void loop(void);

int main(int argc, char** argv) {
    // UNBUFFERED. The game narrates its own start-up over the serial port, and
    // if that narration is sitting in a buffer when something goes wrong it is
    // lost exactly when it was about to be useful.
    setvbuf(stdout, nullptr, _IONBF, 0);

    int scale = 2;
    int frames = 0;   // 0 = run until the window is closed
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sens") && i + 1 < argc) {
            const float v = (float)atof(argv[++i]);
            if (v > 0.0f) g_host_mouse_sens = v;
        }
        else if (!strcmp(argv[i], "--help")) {
            printf("phantom [--scale N] [--sens F] [--frames N]\n"
                   "  --scale N    window size, N times the 480x480 panel (1-4, default 2)\n"
                   "  --sens F     mouse scale, logical pixels per mouse count.\n"
                   "               Default 0.10. Lower is slower. Full deflection\n"
                   "               costs 115/F counts of hand movement.\n"
                   "  --frames N   run N frames and exit -- a smoke test, not a mode\n");
            return 0;
        }
    }

    // OPENED BEFORE setup(), so that a window that cannot be created is reported
    // as itself rather than as the panel failing inside the game's own start-up.
    if (!host_window_open(scale, "PHANTOM")) {
        fprintf(stderr, "could not open a window\n");
        return 1;
    }

    printf("mouse scale %.3f -- full deflection at %.0f mouse counts\n",
           (double)g_host_mouse_sens, (double)(115.0f / g_host_mouse_sens));

    setup();

    int n = 0;
    while (host_window_pump()) {
        loop();
        if (frames && ++n >= frames) break;
    }

    host_window_close();
    return 0;
}
