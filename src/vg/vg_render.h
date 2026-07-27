#pragma once
#include "vg_input.h"

// Submits the whole frame (world + HUD + overlays) to the rasteriser. Does not
// flush -- main.cpp calls vg_rast_flush() so it can time submit and blit apart.
// Takes the input so the HUD can draw the live steering indicator.
void vg_render_frame(const VgInput* in, float fps);
