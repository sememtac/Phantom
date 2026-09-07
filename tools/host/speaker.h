#pragma once
// An APPROXIMATION of the device speaker, shared by score_render.cpp and
// score_live.cpp so that the two previews cannot disagree.
//
// The driver is one centimetre across and gives very little under about 300 Hz,
// which vg_sfx.cpp states and which was found by ear on the device. This removes
// what the driver would not have moved.
//
// WARNING: nobody measured the driver to make this. It has the right shape and
// nothing more. Use it to find notes the device loses. Do not judge level or
// tone from it.
struct Speaker {
    float hp1 = 0, hp2 = 0, in1 = 0, in2 = 0;
    float a = 0;

    void set(float corner, float rate) {
        // One pole, applied twice, for about 12 dB for each octave under the corner.
        const float rc = 1.0f / (6.2831853f * corner);
        a = rc / (rc + 1.0f / rate);
    }

    float step(float x) {
        hp1 = a * (hp1 + x - in1);
        in1 = x;
        hp2 = a * (hp2 + hp1 - in2);
        in2 = hp1;
        return hp2;
    }

    // Filter a block in place, and keep it inside the range of a sample.
    void block(short* pcm, int n) {
        for (int i = 0; i < n; i++) {
            float v = step((float)pcm[i]);
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm[i] = (short)v;
        }
    }
};
