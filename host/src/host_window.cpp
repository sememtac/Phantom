#include "host_window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "cfg_display.h"   // SCR_W, SCR_H -- the panel this window stands in for

static HWND      s_hwnd    = nullptr;
static int       s_scale   = 2;
static bool      s_quit    = false;
static bool      s_focus   = false;
static float     s_mdx     = 0.0f;
static float     s_mdy     = 0.0f;
static uint32_t* s_bgra    = nullptr;   // SCR_W * SCR_H, what GDI actually blits
static BITMAPINFO s_bmi;

// ---------------------------------------------------------------------------
// THE FRAME RATE IS CAPPED, and it is not cosmetic.
//
// On the device the panel paces the game: a band cannot go out faster than the
// wire carries it, and the loop settles around 60. A PC will run this loop as
// fast as it is asked to, and the flight model sub-steps at a fixed 0.02s -- so
// an uncapped window integrates the ship differently from the board and the
// handling stops being the handling. Pacing here keeps the feel comparable,
// which is the entire reason this build exists.
// ---------------------------------------------------------------------------
#define HOST_FRAME_US 16667

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE:
        case WM_DESTROY:      s_quit = true; return 0;
        case WM_SETFOCUS:     s_focus = true;  return 0;
        case WM_KILLFOCUS:    s_focus = false; ShowCursor(TRUE); return 0;
        case WM_KEYDOWN:
            // Escape releases the mouse rather than quitting, because the mouse
            // is captured while flying and a window you cannot get out of is a
            // worse problem than one that will not close.
            if (w == VK_ESCAPE) { s_focus = false; ShowCursor(TRUE); }
            return 0;
        default: break;
    }
    return DefWindowProc(h, m, w, l);
}

bool host_window_open(int scale, const char* title) {
    if (s_hwnd) return true;
    s_scale = (scale < 1) ? 1 : (scale > 4 ? 4 : scale);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PhantomHost";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) return false;

    RECT r = { 0, 0, SCR_W * s_scale, SCR_H * s_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("PhantomHost", title, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (!s_hwnd) return false;

    s_bgra = (uint32_t*)malloc((size_t)SCR_W * SCR_H * 4);
    if (!s_bgra) return false;

    // Top-down: negative height, so row 0 is the top row and the panel's own
    // ordering survives the blit untouched.
    ZeroMemory(&s_bmi, sizeof(s_bmi));
    s_bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth       = SCR_W;
    s_bmi.bmiHeader.biHeight      = -SCR_H;
    s_bmi.bmiHeader.biPlanes      = 1;
    s_bmi.bmiHeader.biBitCount    = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;

    ShowWindow(s_hwnd, SW_SHOW);
    SetForegroundWindow(s_hwnd);
    s_focus = true;
    return true;
}

void host_window_close(void) {
    if (s_bgra) { free(s_bgra); s_bgra = nullptr; }
    if (s_hwnd) { DestroyWindow(s_hwnd); s_hwnd = nullptr; }
}

bool host_window_quit_requested(void) { return s_quit; }
bool host_window_focused(void)        { return s_focus; }

bool host_window_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // THE MOUSE IS CAPTURED AND RE-CENTRED EVERY FRAME. Reading the absolute
    // cursor and putting it back in the middle turns a pointer that would stop
    // at the edge of the desk into one that can be pushed for ever, which is
    // what a finger dragging across a panel does.
    // GetForegroundWindow rather than our own focus flag alone: a window that
    // has been alt-tabbed away from must not still be dragging the pointer back
    // to its centre several times a second.
    if (s_focus && s_hwnd && !s_quit && GetForegroundWindow() == s_hwnd) {
        RECT cr; GetClientRect(s_hwnd, &cr);
        POINT mid = { (cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2 };
        ClientToScreen(s_hwnd, &mid);
        POINT cur; GetCursorPos(&cur);
        s_mdx += (float)(cur.x - mid.x);
        s_mdy += (float)(cur.y - mid.y);
        SetCursorPos(mid.x, mid.y);
        while (ShowCursor(FALSE) >= 0) {}
    }
    return !s_quit;
}

void host_mouse_take_delta(float* dx, float* dy) {
    if (dx) *dx = s_mdx;
    if (dy) *dy = s_mdy;
    s_mdx = s_mdy = 0.0f;
}

bool host_key_down(int vk) {
    if (!s_focus) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void host_window_present(const uint16_t* panel) {
    if (!s_hwnd || !s_bgra || !panel) return;

    // THE PANEL STORES RGB565 BYTE-SWAPPED. VGC() swaps the halves of a native
    // word before it ever reaches a band, so recovering the colour means
    // swapping back first -- see the channel map in vg_tv.cpp, which is the one
    // place this layout is written down.
    const int n = SCR_W * SCR_H;
    for (int i = 0; i < n; i++) {
        const uint16_t s = panel[i];
        const uint16_t v = (uint16_t)((s >> 8) | (s << 8));
        const uint32_t r = (uint32_t)((v >> 11) & 0x1F);
        const uint32_t g = (uint32_t)((v >>  5) & 0x3F);
        const uint32_t b = (uint32_t)( v        & 0x1F);
        // 5/6 bit to 8 by replicating the high bits, so full scale stays full.
        const uint32_t R = (r << 3) | (r >> 2);
        const uint32_t G = (g << 2) | (g >> 4);
        const uint32_t B = (b << 3) | (b >> 2);
        s_bgra[i] = (R << 16) | (G << 8) | B;
    }

    HDC dc = GetDC(s_hwnd);
    RECT cr; GetClientRect(s_hwnd, &cr);
    SetStretchBltMode(dc, COLORONCOLOR);   // no smoothing: these are 1 px lines
    StretchDIBits(dc, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                  0, 0, SCR_W, SCR_H, s_bgra, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);

    // Pace to the device's cadence. Sleep gives back the millisecond it owes and
    // a short spin takes up the remainder, because Sleep(1) is not one
    // millisecond on Windows and a frame that overshoots is a frame the flight
    // model integrates differently.
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER prev = {};
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&prev); }
    for (;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double us = (double)(now.QuadPart - prev.QuadPart) * 1e6 / (double)freq.QuadPart;
        if (us >= (double)HOST_FRAME_US) { prev = now; break; }
        if ((double)HOST_FRAME_US - us > 1500.0) Sleep(1);
    }
}
