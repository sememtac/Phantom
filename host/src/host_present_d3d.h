#pragma once
#include <windows.h>
#include <stdint.h>

// The picture, presented through a flip-model swap chain instead of GDI.
//
// Every one of these is allowed to fail. host_window.cpp keeps the StretchDIBits
// path and uses it whenever host_d3d_ready() is false, so a machine that cannot
// make a swap chain still plays -- it just carries DWM's extra frame or two of
// delay, which is the whole reason this file exists.

// Build the device, the chain and the upload texture for a src_w by src_h
// picture. False means the caller keeps using GDI; nothing is left allocated.
bool host_d3d_init(HWND hwnd, int src_w, int src_h);

// True while the swap chain is usable. Goes false on a lost device.
bool host_d3d_ready(void);

// Draw one frame, letterboxed into the given viewport, and present it.
//
// IT BLOCKS until the display takes the frame, so it is the frame pacer as well
// as the presenter. False means it could not, and the caller should fall back to
// GDI for this frame AND pace itself.
bool host_d3d_present(const uint32_t* bgra, int src_w, int src_h,
                      int vx, int vy, int vw, int vh);

void host_d3d_shutdown(void);
