#include "vg_render.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_screens.h"
#include <stdio.h>
#include <math.h>

// Frame orchestration only. Every actual drawing routine lives in a vg_draw_*
// or vg_hud/vg_overlay module; what remains here is the part that is genuinely
// global -- the draw ORDER, and which layers get the spherical warp.

// Declared here rather than in vg_draw.h because these three are HUD pieces that
// must NOT be warped, so they are called from outside the warp bracket.
void vg_draw_lock_box(const VgCam& cam);
void vg_draw_steer_indicator(const VgInput* in);
void vg_draw_target_markers(const VgCam& cam);
void vg_draw_threat_indicator(const VgCam& cam);

// Frame rate, bottom left, during flight only. The menus sit comfortably above
// 60 and have nothing to report; the number is there to be read while something
// is actually costing frame time. Held clear of the rounded corner by SCR_SAFE
// and drawn last so nothing can cover it.
static void draw_fps(float fps) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f));
    vg_text(SCR_SAFE, SCR_H - SCR_SAFE - 14, buf,
            fps >= 59.0f ? INK_BRIGHT : INK_FAINT, 2);
}

void vg_render_frame(const VgInput* in, float fps) {
    VgCam cam = vg_cam_make(vg.bank, vg.shake_x, vg.shake_y);

    vg_rast_begin_frame();

    // World, back to front. The boundary goes down before anything solid so the
    // hidden-line fills occlude it.
    vg_draw_starfield(cam);
    vg_draw_arena_grid(cam);
    vg_draw_world(cam);

    // Menus fly the idle scene underneath but carry no instruments -- a HUD on
    // the ship-select screen would be reporting on a fight that is not happening.
    if (vg_state_is_menu(vg.state)) {
        switch (vg.state) {
        case VG_ENTRY:   vg_draw_entry();    break;
        case VG_SELECT:  vg_draw_select();   break;
        case VG_BRACKET: vg_draw_bracket();  break;
        case VG_REPAIR:  vg_draw_repair();   break;
        default:         vg_draw_overlays(); break;
        }
        return;
    }

    vg_draw_lock_box(cam);

    // Instruments are drawn onto the virtual canopy. The bend tracks speed, so
    // the canopy visibly flexes as you open the throttle -- but QUANTISED,
    // because a continuously varying warp shifts every HUD line by a fraction of
    // a pixel per frame, which reads as shimmer rather than motion.
    // Driven by the throttle COMMAND, not by actual speed: the canopy responds
    // to what you asked for rather than waiting out the flight model's
    // deliberately slow acceleration.
    float sn = vg.throttle_vis;
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;

    float warp = HUD_WARP_SPEED_MIN + (1.0f - HUD_WARP_SPEED_MIN) * sn;
    warp = floorf(warp * HUD_WARP_STEPS + 0.5f) / HUD_WARP_STEPS;

    vg_hud_warp(true, warp);
    vg_draw_hud(cam, in, fps);
    vg_hud_warp(false, 1.0f);

    // Everything past here stays FLAT: the steering ring has to sit exactly under
    // the finger, and the markers have to line up with what they point at. A
    // curved control that does not match where you are touching feels broken
    // rather than physical.
    vg_draw_steer_indicator(in);
    vg_draw_target_markers(cam);
    vg_draw_threat_indicator(cam);

    vg_draw_overlays();

    // Last of all, over everything including the instruments.
    if (vg.state == VG_PAUSE) vg_draw_pause();

    draw_fps(fps);
}
