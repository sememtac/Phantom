#include "host_window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "cfg_display.h"   // SCR_W, SCR_H -- the panel this window stands in for

static HWND      s_hwnd    = nullptr;
static int       s_scale   = 2;
static bool      s_quit    = false;
static uint32_t* s_bgra    = nullptr;   // SCR_W * SCR_H, what GDI actually blits
static BITMAPINFO s_bmi;
static bool       s_capture = true;

// THE VIEWPORT: where the picture actually sits inside the client area.
//
// The panel is square and a window is whatever the player drags it to, so the
// picture is letterboxed -- the largest rectangle of the panel's aspect that
// fits, centred, with black either side. Stretching to the client instead would
// make a resize squash the game, and a circle drawn by the game has to stay a
// circle or nothing on the HUD can be trusted.
//
// Computed in one place because TWO things need it and they must agree: the
// blit, and the pointer. A menu click maps back through this, so if the two ever
// disagreed the cursor would select something other than what it is over.
struct HostView { int x, y, w, h; };

static HostView view_of(HWND h) {
    RECT cr; GetClientRect(h, &cr);
    const int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    HostView v = { 0, 0, cw, ch };
    if (cw <= 0 || ch <= 0) return v;
    // Fit by whichever axis runs out first.
    if (cw * SCR_H > ch * SCR_W) { v.h = ch; v.w = ch * SCR_W / SCR_H; }
    else                          { v.w = cw; v.h = cw * SCR_H / SCR_W; }
    v.x = (cw - v.w) / 2;
    v.y = (ch - v.h) / 2;
    return v;
}
static float      s_rawx    = 0.0f;   // device counts since the last read
static float      s_rawy    = 0.0f;

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
        case WM_KILLFOCUS:    ShowCursor(TRUE); return 0;
        case WM_INPUT: {
            // Only relative reports are steering. A tablet or a remote-desktop
            // session sends absolute ones, and treating those as a delta would
            // throw the stick to a corner on the first packet.
            UINT sz = 0;
            GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
            if (sz && sz <= sizeof(RAWINPUT)) {
                RAWINPUT ri;
                if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz,
                                    sizeof(RAWINPUTHEADER)) == sz &&
                    ri.header.dwType == RIM_TYPEMOUSE &&
                    !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    s_rawx += (float)ri.data.mouse.lLastX;
                    s_rawy += (float)ri.data.mouse.lLastY;
                }
            }
            return DefWindowProc(h, m, w, l);
        }
        case WM_ERASEBKGND:
            // The presenter paints every pixel of the client every frame, so
            // letting Windows erase it first only adds a flash of white.
            return 1;

        case WM_GETMINMAXINFO: {
            // Small enough to be useful, large enough that the letterbox
            // arithmetic never divides into nothing.
            MINMAXINFO* mm = (MINMAXINFO*)l;
            RECT t = { 0, 0, 240, 240 };
            AdjustWindowRect(&t, (DWORD)GetWindowLongPtr(h, GWL_STYLE), FALSE);
            mm->ptMinTrackSize.x = t.right - t.left;
            mm->ptMinTrackSize.y = t.bottom - t.top;
            return 0;
        }

        case WM_SIZING: {
            // HOLD THE ASPECT WHILE IT IS BEING DRAGGED, so the window itself
            // tells the player the shape is fixed. Letterboxing alone would let
            // them drag a long thin window and watch the game sit in the middle
            // of it, which looks like a bug rather than a rule.
            RECT* r = (RECT*)l;
            RECT pad = { 0, 0, 100, 100 };
            AdjustWindowRect(&pad, (DWORD)GetWindowLongPtr(h, GWL_STYLE), FALSE);
            const int padx = (pad.right - pad.left) - 100;
            const int pady = (pad.bottom - pad.top) - 100;

            int cw = (r->right - r->left) - padx;
            int ch = (r->bottom - r->top) - pady;
            if (cw < 1) cw = 1;
            if (ch < 1) ch = 1;

            const bool horiz = (w == WMSZ_LEFT || w == WMSZ_RIGHT);
            const bool vert  = (w == WMSZ_TOP  || w == WMSZ_BOTTOM);
            if (horiz)      ch = cw * SCR_H / SCR_W;   // dragging a side: width leads
            else if (vert)  cw = ch * SCR_W / SCR_H;   // dragging top or bottom
            else if (cw * SCR_H > ch * SCR_W) ch = cw * SCR_H / SCR_W;  // a corner:
            else                              cw = ch * SCR_W / SCR_H;  // follow the larger

            // Move only the edges the player has hold of.
            if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT)
                 r->left = r->right - (cw + padx);
            else r->right = r->left + (cw + padx);

            if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT)
                 r->top = r->bottom - (ch + pady);
            else r->bottom = r->top + (ch + pady);
            return TRUE;
        }

        case WM_KEYDOWN:
            // Escape is the PWR key now -- the menu key -- so it is read like
            // any other key rather than acted on here.
            //
            // IT USED TO CLEAR s_focus, and that was a latch nothing could set
            // again: WM_SETFOCUS only arrives when a window GAINS keyboard
            // focus, and a window that already has it never gains it. One press
            // of Escape therefore killed every key and every mouse button for
            // the rest of the session. That is what "the left click does not
            // fire" turned out to be. Focus is asked for now, not remembered.
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

    // RAW MOUSE INPUT, and this is what makes the stick honest.
    //
    // GetCursorPos deltas are what the POINTER did, which is the device's motion
    // after Windows has applied pointer speed and "enhance pointer precision".
    // That last one is acceleration: the same hand movement produces a different
    // deflection depending on how fast it was made. A flight control cannot be
    // calibrated against that, because there is no single number to calibrate.
    //
    // WM_INPUT reports what the DEVICE reported, in its own counts, before any of
    // that. No RIDEV_INPUTSINK: input while another window is in front is not
    // wanted.
    {
        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01;   // generic desktop
        rid.usUsage     = 0x02;   // mouse
        rid.dwFlags     = 0;
        rid.hwndTarget  = s_hwnd;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
    }

    ShowWindow(s_hwnd, SW_SHOW);
    SetForegroundWindow(s_hwnd);
    return true;
}

void host_window_close(void) {
    if (s_bgra) { free(s_bgra); s_bgra = nullptr; }
    if (s_hwnd) { DestroyWindow(s_hwnd); s_hwnd = nullptr; }
}

bool host_window_quit_requested(void) { return s_quit; }
// ASKED, NOT REMEMBERED. A latch was wrong twice over: it could be cleared with
// no event able to set it again, and it could disagree with the window manager
// after an alt-tab. The system already knows the answer.
bool host_window_focused(void) {
    return s_hwnd != nullptr && GetForegroundWindow() == s_hwnd;
}

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
    if (s_capture && s_hwnd && !s_quit && host_window_focused()) {
        const HostView v = view_of(s_hwnd);
        POINT mid = { v.x + v.w / 2, v.y + v.h / 2 };
        ClientToScreen(s_hwnd, &mid);
        // The pointer is still parked in the middle every frame, but only so it
        // cannot wander onto another window and take a click with it. The motion
        // itself comes from WM_INPUT, not from where the pointer ended up.
        SetCursorPos(mid.x, mid.y);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        while (ShowCursor(TRUE) < 0) {}
    }
    return !s_quit;
}

void host_window_set_capture(bool on) {
    if (s_capture == on) return;
    s_capture = on;
    // Drop whatever movement arrived under the other regime, or the first frame
    // after a menu closes would fling the stick by however far the pointer
    // wandered while it was free.
    s_rawx = s_rawy = 0.0f;
}

bool host_mouse_logical(float* x, float* y) {
    if (!s_hwnd) return false;
    POINT p; GetCursorPos(&p);
    ScreenToClient(s_hwnd, &p);
    // Through the viewport, not the client: with a letterbox the two differ, and
    // a pointer mapped through the wrong one selects something other than what it
    // is sitting on.
    const HostView v = view_of(s_hwnd);
    if (v.w <= 0 || v.h <= 0) return false;
    const int px = p.x - v.x, py = p.y - v.y;
    if (px < 0 || py < 0 || px >= v.w || py >= v.h) return false;   // on a bar
    if (x) *x = (float)px * (float)SCR_W / (float)v.w;
    if (y) *y = (float)py * (float)SCR_H / (float)v.h;
    return true;
}

void host_mouse_take_delta(float* dx, float* dy) {
    if (dx) *dx = s_rawx;
    if (dy) *dy = s_rawy;
    s_rawx = s_rawy = 0.0f;
}

bool host_key_down(int vk) {
    if (!host_window_focused()) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void host_window_present(const uint16_t* panel) {
    if (!s_hwnd || !s_bgra || !panel) return;

    // THE PANEL STORES RGB565 BYTE-SWAPPED. VGC() swaps the halves of a native
    // word before it ever reaches a band, so recovering the colour means
    // swapping back first -- see the channel map in vg_tv.cpp, which is the one
    // place this layout is written down.
    // TURNED BACK. The game draws through a quarter turn on its way to the panel
    // -- rot_pt in vg_raster.cpp sends logical (x,y) to panel (y, SCR_H-1-x) --
    // because the glass is mounted a quarter turn off. A window has no such
    // excuse, so the turn is undone here and the picture stands up.
    //
    // Reading logical (lx,ly) means fetching panel[(SCR_H-1-lx)*W + ly]. Only the
    // PICTURE needs this: vg_touch_read already reports in the logical frame on
    // both ports, so the pointer needs no turn of its own.
    for (int ly = 0; ly < SCR_H; ly++)
    for (int lx = 0; lx < SCR_W; lx++) {
        const uint16_t s = panel[(size_t)(SCR_H - 1 - lx) * SCR_W + ly];
        const int i = ly * SCR_W + lx;
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
    const HostView v = view_of(s_hwnd);

    // The bars, and only the bars. Painting the whole client and then drawing
    // over it would flash the picture off and on again every frame.
    if (v.x > 0 || v.y > 0) {
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT b;
        if (v.x > 0) {
            b = { 0, 0, v.x, cr.bottom };                    FillRect(dc, &b, black);
            b = { v.x + v.w, 0, cr.right, cr.bottom };       FillRect(dc, &b, black);
        }
        if (v.y > 0) {
            b = { 0, 0, cr.right, v.y };                     FillRect(dc, &b, black);
            b = { 0, v.y + v.h, cr.right, cr.bottom };       FillRect(dc, &b, black);
        }
    }

    SetStretchBltMode(dc, COLORONCOLOR);   // no smoothing: these are 1 px lines
    StretchDIBits(dc, v.x, v.y, v.w, v.h,
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
