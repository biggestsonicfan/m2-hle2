/*
 * kiosk.h — capture mode: a chrome-free window at a fixed capture resolution,
 * parked off the desktop, driven from a notification-area (systray) icon.
 *
 * Why this exists. OBS's Game Capture hooks a process's swapchain Present, so
 * it needs a window to attach to: --headless (no window, no GPU context at all)
 * cannot be recorded, only scripted. Capture mode is the other half of that —
 * the emulator keeps a real D3D11 window at exactly the recording resolution,
 * but the window has no title bar to close, no minimise box, and by default
 * sits outside the virtual desktop where nothing on screen can land on it. OBS
 * finds it by name and captures its frames wherever it is; the tray icon is the
 * only thing the desktop shows, and the only way back out.
 *
 * Board-independent host shell code: nothing here knows what is being emulated.
 *
 * Things that are load-bearing rather than decorative:
 *  - The window must stay VISIBLE in the Win32 sense. OBS's window enumeration
 *    (window-helpers.c) drops anything failing IsWindowVisible() or carrying
 *    WS_EX_TOOLWINDOW, so ShowWindow(SW_HIDE) or a tool-window style would hide
 *    it from the capture source list. Parking it off the virtual desktop keeps
 *    it visible to that test and invisible to the user.
 *  - A minimised window stops presenting, which freezes the capture. Windows
 *    can minimise us without a WM_SYSCOMMAND (Win+D, "show desktop"), so
 *    SIZE_MINIMIZED is undone as well as blocked.
 *  - Quit is gated on the tray's own Exit item: WM_CLOSE from Alt+F4 or the
 *    taskbar is swallowed while capture mode is on, which is the point.
 *
 * THE HEADLESS TRAY (the second half of this file) is the same notification
 * area icon for a run that has no window at all. --headless was a process with
 * no window, no console of its own once its launcher goes, and nothing on the
 * desktop that says it exists -- so the only way to stop one was Task Manager,
 * and an orphan would sit there holding its ports and its ROM. It gets the
 * same icon, and the two menu items that matter without a keyboard: Restart
 * sound board, and Exit. NOT pause -- see tray__menu for why.
 */
#ifndef KIOSK_H
#define KIOSK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_icon.h"
#include "log.h"
#include "sokol_app.h"

#define KIOSK_DEFAULT_WIDTH  1920
#define KIOSK_DEFAULT_HEIGHT 1080

/* Run/pause lives in the emu thread, which this header has no business owning;
 * main.c passes it in so the tray menu can drive it. */
typedef struct {
    bool  (*is_running)(void *ud);
    void  (*set_running)(void *ud, bool run);
    /* Reboot the sound board. Capture mode is exactly where this is needed and
     * nowhere else can reach: the window is parked off the desktop so it cannot
     * take a keystroke, and the debug bridge may already have its one client. */
    void  (*restart_sound)(void *ud);
    void   *ud;
} kiosk_hooks_t;

static struct {
    bool on;              /* capture mode active: no ImGui, window pinned */
    bool pending;         /* asked for before sokol had made the window */
    bool have_window;
    bool shown;           /* parked on screen rather than off the desktop */
    bool quit_ok;         /* Exit chosen from the tray: let the close through */
    int  width, height;
    float fps;            /* presented frames per second, for the tray tooltip */
    char label[96];       /* what the game is, for the tray tooltip and menu */
    kiosk_hooks_t hooks;
} g_kiosk = { .width = KIOSK_DEFAULT_WIDTH, .height = KIOSK_DEFAULT_HEIGHT };

static inline bool kiosk_active(void)       { return g_kiosk.on; }
static inline bool kiosk_quit_allowed(void) { return !g_kiosk.on || g_kiosk.quit_ok; }
/* Capture mode swallows WM_CLOSE so a stray Alt+F4 cannot take a stream down;
 * this is how something that really does mean it says so. The tray's Exit sets
 * the same flag. */
static inline void kiosk_allow_quit(void) { g_kiosk.quit_ok = true; }
static inline void kiosk_set_hooks(const kiosk_hooks_t *h) { if (h) g_kiosk.hooks = *h; }

#if defined(_WIN32)

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

#define KIOSK_WM_TRAY    (WM_APP + 0x31)
#define KIOSK_WM_UNMIN   (WM_APP + 0x32)
#define KIOSK_ID_SHOW    1
#define KIOSK_ID_RUN     2
#define KIOSK_ID_LEAVE   3
#define KIOSK_ID_EXIT    4
#define KIOSK_ID_SNDRST  5

static struct {
    HWND      hwnd;
    WNDPROC   prev_proc;
    HICON     icon_small, icon_big;
    bool      tray_added;
    bool      ballooned;
    UINT      taskbar_created;   /* re-add the icon if Explorer restarts */
    LONG_PTR  saved_style;
    RECT      saved_rect;
    float     acc_time;          /* frame-rate accumulator, one-second window */
    int       acc_frames;
    int       since_log;
} g_kioskw;

static inline void kiosk__wide(const char *utf8, wchar_t *out, int out_chars) {
    out[0] = 0;
    if (utf8 && utf8[0]) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars);
    out[out_chars - 1] = 0;
}

/* Build an HICON at `size` from the same art the window icon uses. */
static inline HICON kiosk__make_icon(int size) {
    uint8_t *rgba = (uint8_t *)malloc((size_t)size * (size_t)size * 4);
    if (!rgba) return NULL;
    app_icon_render(size, rgba);

    BITMAPV5HEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;          /* top-down */
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    void *bits = NULL;
    HDC     dc     = GetDC(NULL);
    HBITMAP colour = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!colour || !bits) { free(rgba); if (colour) DeleteObject(colour); return NULL; }

    uint8_t *dst = (uint8_t *)bits;
    for (int i = 0; i < size * size; i++) {           /* RGBA -> BGRA */
        dst[i * 4 + 0] = rgba[i * 4 + 2];
        dst[i * 4 + 1] = rgba[i * 4 + 1];
        dst[i * 4 + 2] = rgba[i * 4 + 0];
        dst[i * 4 + 3] = rgba[i * 4 + 3];
    }
    free(rgba);

    /* An all-zero AND mask means "take the colour pixel", so the 32-bit alpha
     * channel is what shapes the icon. */
    const int mask_stride = ((size + 15) / 16) * 2;
    uint8_t  *mask_bits   = (uint8_t *)calloc(1, (size_t)mask_stride * (size_t)size);
    HBITMAP   mask        = CreateBitmap(size, size, 1, 1, mask_bits);
    free(mask_bits);

    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = colour;
    HICON icon = CreateIconIndirect(&ii);
    if (mask)   DeleteObject(mask);
    if (colour) DeleteObject(colour);
    return icon;
}

/* ---- the notification-area icon itself ------------------------------------
 *
 * Capture mode hangs an icon on the sokol window; a headless run hangs one on a
 * hidden window of its own (the second half of this file). What differs between
 * them is the window, the menu and where the frame rate comes from -- but the
 * Shell_NotifyIcon conversation is the same one twice, so it is written once
 * here and each side keeps only its own tooltip text, which is the part worth
 * saying differently. Both were separate copies to begin with and they had
 * already drifted, which is the argument.
 *
 * The `added` flag stays with the caller rather than being hidden in here: each
 * tray already has one, and the wndprocs clear it directly when Explorer
 * restarts. */
static inline void kiosk__icon_desc(HWND hwnd, HICON icon, UINT cb_msg,
                                    const char *tip, NOTIFYICONDATAW *nid) {
    memset(nid, 0, sizeof(*nid));
    nid->cbSize           = sizeof(*nid);
    nid->hWnd             = hwnd;
    nid->uID              = 1;
    nid->uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid->uCallbackMessage = cb_msg;
    nid->hIcon            = icon;
    kiosk__wide(tip ? tip : "m2-hle", nid->szTip,
                (int)(sizeof(nid->szTip) / sizeof(wchar_t)));
}

/* NIM_ADD, then NOTIFYICON_VERSION_4 so the callback carries the cursor
 * position. Answers whether the shell took it; `whynot` is the caller's own
 * warning for when it did not -- a service, a Session 0 run, or a machine with
 * no shell -- because looking for an icon that is never coming is worse than
 * being told it is not. */
static inline bool kiosk__icon_put(HWND hwnd, HICON icon, UINT cb_msg,
                                   const char *tip, const char *whynot) {
    if (!hwnd) return false;
    NOTIFYICONDATAW nid;
    kiosk__icon_desc(hwnd, icon, cb_msg, tip, &nid);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        if (whynot) LOG_WARN("%s", whynot);
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

static inline void kiosk__icon_retip(HWND hwnd, HICON icon, UINT cb_msg,
                                     const char *tip) {
    NOTIFYICONDATAW nid;
    kiosk__icon_desc(hwnd, icon, cb_msg, tip, &nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static inline void kiosk__icon_drop(HWND hwnd) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* A one-off balloon, on an icon that is already up. */
static inline void kiosk__icon_balloon(HWND hwnd, const char *title, const char *body) {
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = hwnd;
    nid.uID         = 1;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_NONE | NIIF_NOSOUND;
    kiosk__wide(title, nid.szInfoTitle,
                (int)(sizeof(nid.szInfoTitle) / sizeof(wchar_t)));
    kiosk__wide(body, nid.szInfo, (int)(sizeof(nid.szInfo) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* ---- capture mode's tray ---------------------------------------------------- */

/* What the tooltip and the menu header say about the capture window. */
static inline void kiosk__tray_tip(char *out, int cap) {
    const bool running = g_kiosk.hooks.is_running ? g_kiosk.hooks.is_running(g_kiosk.hooks.ud)
                                                  : false;
    snprintf(out, (size_t)cap, "m2-hle%s%s - capture %dx%d, %s, %.0f fps",
             g_kiosk.label[0] ? " - " : "", g_kiosk.label,
             g_kiosk.width, g_kiosk.height,
             running ? "running" : "paused", (double)g_kiosk.fps);
}

static inline void kiosk__tray_update(void) {
    if (!g_kioskw.tray_added) return;
    char tip[160];
    kiosk__tray_tip(tip, (int)sizeof tip);
    kiosk__icon_retip(g_kioskw.hwnd, g_kioskw.icon_small, KIOSK_WM_TRAY, tip);
}

/*
 * Called once per rendered frame while capture mode is on. A parked window has
 * no visible sign of life, and "is it still presenting" is the one question
 * worth being able to answer — a minimised or stalled window is a frozen OBS
 * source. The rate goes in the tooltip (hover the tray icon) and, less often,
 * in the log.
 */
static inline void kiosk_frame_tick(float dt) {
    if (!g_kiosk.on) return;
    g_kioskw.acc_time   += (dt > 0.0f && dt < 1.0f) ? dt : (1.0f / 60.0f);
    g_kioskw.acc_frames += 1;
    if (g_kioskw.acc_time < 1.0f) return;

    g_kiosk.fps = (float)g_kioskw.acc_frames / g_kioskw.acc_time;
    g_kioskw.acc_time = 0.0f;
    g_kioskw.acc_frames = 0;
    kiosk__tray_update();

    if (++g_kioskw.since_log >= 30) {
        g_kioskw.since_log = 0;
        LOG_INFO("capture mode: %dx%d, %.1f fps presented, window %s",
                 g_kiosk.width, g_kiosk.height, g_kiosk.fps,
                 g_kiosk.shown ? "on screen" : "parked");
    }
}

static inline void kiosk__tray_add(void) {
    if (g_kioskw.tray_added || !g_kioskw.hwnd) return;
    if (!g_kioskw.icon_small) {
        int sm = GetSystemMetrics(SM_CXSMICON); if (sm < 8) sm = 16;
        g_kioskw.icon_small = kiosk__make_icon(sm);
    }
    char tip[160];
    kiosk__tray_tip(tip, (int)sizeof tip);
    if (!kiosk__icon_put(g_kioskw.hwnd, g_kioskw.icon_small, KIOSK_WM_TRAY, tip,
                         "capture mode: the tray icon could not be added (Shell_NotifyIcon failed)"))
        return;
    g_kioskw.tray_added = true;

    if (!g_kioskw.ballooned) {
        g_kioskw.ballooned = true;
        char body[256];
        snprintf(body, sizeof(body),
                 "Running hidden at %dx%d. Capture m2hle.exe in OBS. "
                 "Right-click this icon to show the window or exit.",
                 g_kiosk.width, g_kiosk.height);
        kiosk__icon_balloon(g_kioskw.hwnd, "m2-hle capture mode", body);
    }
}

static inline void kiosk__tray_remove(void) {
    if (!g_kioskw.tray_added) return;
    kiosk__icon_drop(g_kioskw.hwnd);
    g_kioskw.tray_added = false;
}

/* Place the capture window: client area exactly width x height, either at the
 * top-left of the desktop or just past its right edge where nothing can reach
 * it. Off-desktop is not hidden — Present, and so the capture hook, run on. */
static inline void kiosk__place(bool activate) {
    if (!g_kioskw.hwnd) return;
    int x, y;
    if (g_kiosk.shown) {
        x = 0; y = 0;
    } else {
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    }
    RECT rc = { 0, 0, g_kiosk.width, g_kiosk.height };
    const LONG_PTR style   = GetWindowLongPtrW(g_kioskw.hwnd, GWL_STYLE);
    const LONG_PTR exstyle = GetWindowLongPtrW(g_kioskw.hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, (DWORD)style, FALSE, (DWORD)exstyle);
    SetWindowPos(g_kioskw.hwnd, HWND_TOP, x, y, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | (activate ? 0u : (UINT)SWP_NOACTIVATE));
    if (activate) { SetForegroundWindow(g_kioskw.hwnd); SetFocus(g_kioskw.hwnd); }
}

static inline void kiosk__apply(void) {
    if (!g_kioskw.hwnd) return;
    LONG_PTR style = g_kioskw.saved_style;
    style &= ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(g_kioskw.hwnd, GWL_STYLE, style);
    kiosk__place(g_kiosk.shown);
    kiosk__tray_add();
    /* Only claim the icon when the shell actually took it -- kiosk__tray_add
     * warns when it did not, and a line saying it is up next to a warning that
     * it is not is worse than either. The headless tray gates its own the same
     * way. */
    LOG_INFO("capture mode: %dx%d, window %s, tray icon %s",
             g_kiosk.width, g_kiosk.height,
             g_kiosk.shown ? "on screen" : "parked off the desktop",
             g_kioskw.tray_added ? "up" : "UNAVAILABLE");
}

static inline void kiosk__restore_window(void) {
    if (!g_kioskw.hwnd) return;
    SetWindowLongPtrW(g_kioskw.hwnd, GWL_STYLE, g_kioskw.saved_style);
    RECT r = g_kioskw.saved_rect;
    if (r.right - r.left < 320 || r.bottom - r.top < 240) {
        r.left = 64; r.top = 64; r.right = 64 + 1280; r.bottom = 64 + 720;
    }
    SetWindowPos(g_kioskw.hwnd, HWND_TOP, r.left, r.top,
                 r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    ShowWindow(g_kioskw.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_kioskw.hwnd);
}

static inline void kiosk_show_window(bool show) {
    if (g_kiosk.shown == show) return;
    g_kiosk.shown = show;
    if (!g_kiosk.on) return;
    kiosk__place(show);
    LOG_INFO("capture mode: window %s", show ? "shown at 0,0" : "parked off the desktop");
}

static inline void kiosk_leave(void) {
    if (!g_kiosk.on) return;
    g_kiosk.on      = false;
    g_kiosk.pending = false;
    g_kiosk.shown   = true;
    kiosk__tray_remove();
    kiosk__restore_window();
    LOG_INFO("capture mode off: the normal window and its menus are back");
}

static inline void kiosk__menu(int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;

    char    line[192];
    wchar_t wide[192];
    snprintf(line, sizeof(line), "m2-hle - %s", g_kiosk.label[0] ? g_kiosk.label : "no ROM");
    kiosk__wide(line, wide, 192);
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, wide);
    snprintf(line, sizeof(line), "capture %dx%d", g_kiosk.width, g_kiosk.height);
    kiosk__wide(line, wide, 192);
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, wide);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    const bool running = g_kiosk.hooks.is_running ? g_kiosk.hooks.is_running(g_kiosk.hooks.ud)
                                                  : false;
    AppendMenuW(m, MF_STRING | (g_kiosk.shown ? MF_CHECKED : 0), KIOSK_ID_SHOW, L"Show window");
    AppendMenuW(m, MF_STRING | (running ? MF_CHECKED : 0) |
                   (g_kiosk.hooks.set_running ? 0 : (MF_GRAYED | MF_DISABLED)),
                KIOSK_ID_RUN, L"Run emulation");
    AppendMenuW(m, MF_STRING | (g_kiosk.hooks.restart_sound ? 0 : (MF_GRAYED | MF_DISABLED)),
                KIOSK_ID_SNDRST, L"Restart sound board");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, KIOSK_ID_LEAVE, L"Leave capture mode");
    AppendMenuW(m, MF_STRING, KIOSK_ID_EXIT,  L"Exit");

    /* The classic tray-menu dance: take the foreground first so clicking away
     * dismisses the menu, and post a null message after so it closes. */
    SetForegroundWindow(g_kioskw.hwnd);
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                          x, y, 0, g_kioskw.hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(g_kioskw.hwnd, WM_NULL, 0, 0);

    switch (cmd) {
        case KIOSK_ID_SHOW: kiosk_show_window(!g_kiosk.shown); break;
        case KIOSK_ID_RUN:
            if (g_kiosk.hooks.set_running) g_kiosk.hooks.set_running(g_kiosk.hooks.ud, !running);
            break;
        case KIOSK_ID_SNDRST:
            if (g_kiosk.hooks.restart_sound) g_kiosk.hooks.restart_sound(g_kiosk.hooks.ud);
            break;
        case KIOSK_ID_LEAVE: kiosk_leave(); break;
        case KIOSK_ID_EXIT:
            g_kiosk.quit_ok = true;
            kiosk__tray_remove();
            sapp_request_quit();
            break;
        default: break;
    }
}

static inline LRESULT CALLBACK kiosk__wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_kioskw.taskbar_created && msg == g_kioskw.taskbar_created && g_kiosk.on) {
        g_kioskw.tray_added = false;      /* Explorer restarted; put it back */
        kiosk__tray_add();
        return 0;
    }
    switch (msg) {
        case KIOSK_WM_TRAY: {
            const UINT ev = LOWORD(lp);
            if (ev == WM_CONTEXTMENU || ev == WM_RBUTTONUP)
                kiosk__menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            else if (ev == NIN_SELECT || ev == NIN_KEYSELECT ||
                     ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK)
                kiosk_show_window(!g_kiosk.shown);
            return 0;
        }
        case KIOSK_WM_UNMIN:
            if (g_kiosk.on) {
                ShowWindow(h, SW_SHOWNOACTIVATE);
                kiosk__place(false);
            }
            return 0;
        case WM_SIZE:
            /* Win+D and the taskbar can minimise us without a WM_SYSCOMMAND,
             * and a minimised window stops presenting — a frozen OBS source.
             * Undo it on the next message rather than from inside WM_SIZE. */
            if (g_kiosk.on && wp == SIZE_MINIMIZED && !g_kiosk.quit_ok)
                PostMessageW(h, KIOSK_WM_UNMIN, 0, 0);
            break;
        case WM_WINDOWPOSCHANGING:
            if (g_kiosk.on && !g_kiosk.quit_ok) {
                WINDOWPOS *wpos = (WINDOWPOS *)lp;
                if (wpos) wpos->flags &= ~(UINT)SWP_HIDEWINDOW;
            }
            break;
        case WM_SYSCOMMAND:
            if (g_kiosk.on && !g_kiosk.quit_ok) {
                switch (wp & 0xFFF0) {
                    case SC_MINIMIZE: case SC_MAXIMIZE: case SC_RESTORE:
                    case SC_SIZE:     case SC_MOVE:     case SC_CLOSE:
                        return 0;      /* the whole point of capture mode */
                    default: break;
                }
            }
            break;
        case WM_CLOSE:
            /* Alt+F4, the taskbar's Close: only the tray's Exit gets through. */
            if (g_kiosk.on && !g_kiosk.quit_ok) return 0;
            break;
        default: break;
    }
    return CallWindowProcW(g_kioskw.prev_proc, h, msg, wp, lp);
}

/* Called once from init(), after sokol has created the window. */
static inline void kiosk_window_ready(void) {
    g_kioskw.hwnd = (HWND)sapp_win32_get_hwnd();
    if (!g_kioskw.hwnd) { LOG_WARN("capture mode: no window handle"); return; }
    g_kiosk.have_window      = true;
    g_kioskw.saved_style     = GetWindowLongPtrW(g_kioskw.hwnd, GWL_STYLE);
    GetWindowRect(g_kioskw.hwnd, &g_kioskw.saved_rect);
    g_kioskw.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_kioskw.prev_proc = (WNDPROC)SetWindowLongPtrW(g_kioskw.hwnd, GWLP_WNDPROC,
                                                    (LONG_PTR)kiosk__wndproc);
    /* A big icon for Alt+Tab and the taskbar, from the same art as the tray. */
    if (!g_kioskw.icon_big) {
        int big = GetSystemMetrics(SM_CXICON); if (big < 16) big = 32;
        g_kioskw.icon_big = kiosk__make_icon(big);
        if (g_kioskw.icon_big)
            SendMessageW(g_kioskw.hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_kioskw.icon_big);
    }
    if (g_kiosk.pending) { g_kiosk.pending = false; kiosk__apply(); }
}

static inline void kiosk_enter(int w, int h, bool show) {
    if (w > 0) g_kiosk.width  = w;
    if (h > 0) g_kiosk.height = h;
    g_kiosk.shown   = show;
    g_kiosk.on      = true;
    g_kiosk.quit_ok = false;
    if (g_kiosk.have_window) kiosk__apply();
    else                     g_kiosk.pending = true;
}

static inline void kiosk_shutdown(void) {
    kiosk__tray_remove();
    if (g_kioskw.hwnd && g_kioskw.prev_proc) {
        SetWindowLongPtrW(g_kioskw.hwnd, GWLP_WNDPROC, (LONG_PTR)g_kioskw.prev_proc);
        g_kioskw.prev_proc = NULL;
    }
    if (g_kioskw.icon_small) { DestroyIcon(g_kioskw.icon_small); g_kioskw.icon_small = NULL; }
    if (g_kioskw.icon_big)   { DestroyIcon(g_kioskw.icon_big);   g_kioskw.icon_big   = NULL; }
}

/* ======================================================================== *
 *  The headless tray
 *
 *  A --headless run has no window, so it needs one of its own to hang the
 *  icon on. Two details decide the shape of it:
 *
 *   - It is a HIDDEN TOP-LEVEL window, not a message-only (HWND_MESSAGE) one.
 *     Explorer announces its restart with a BROADCAST, and broadcasts are not
 *     delivered to message-only windows -- so an icon on one would vanish for
 *     good the first time Explorer restarted, which is exactly the situation
 *     where you want a way to stop the process. WS_EX_TOOLWINDOW keeps it off
 *     the taskbar and Alt+Tab, and it is never shown.
 *
 *   - Exit does NOT call exit(). It raises a flag the headless loop reads, so
 *     the board, the A/V server and the netplay session are taken down in the
 *     same order they are on any other exit. Killing the process from a tray
 *     callback would leave the A/V writer thread sending out of buffers being
 *     freed under it.
 * ======================================================================== */

#define TRAY_WM_ICON    (WM_APP + 0x41)
#define TRAY_ID_SNDRST  2
#define TRAY_ID_EXIT    3

static struct {
    HWND     hwnd;
    HICON    icon;
    bool     started;
    bool     tray_added;
    bool     exit_requested;
    UINT     taskbar_created;
    char     note[128];       /* "--mcp 7172, --av-port 7180", for the tooltip */
    float    fps;             /* board frames per second                       */
} g_trayhl;

static inline bool tray_exit_requested(void) { return g_trayhl.exit_requested; }

/* Which ports this process is answering on goes in the tooltip, so the icon
 * says WHICH emulator it belongs to when several are running. */
static inline void tray__tip(char *out, int cap) {
    const bool running = g_kiosk.hooks.is_running ? g_kiosk.hooks.is_running(g_kiosk.hooks.ud)
                                                  : false;
    snprintf(out, (size_t)cap, "m2-hle headless%s%s - %s, %.0f fps%s%s",
             g_kiosk.label[0] ? " - " : "", g_kiosk.label,
             running ? "running" : "paused", (double)g_trayhl.fps,
             g_trayhl.note[0] ? " - " : "", g_trayhl.note);
}

static inline void tray__update(void) {
    if (!g_trayhl.tray_added) return;
    char tip[160];
    tray__tip(tip, (int)sizeof tip);
    kiosk__icon_retip(g_trayhl.hwnd, g_trayhl.icon, TRAY_WM_ICON, tip);
}

static inline void tray__add(void) {
    if (g_trayhl.tray_added || !g_trayhl.hwnd) return;
    char tip[160];
    tray__tip(tip, (int)sizeof tip);
    /* No icon is not fatal: the emulator is perfectly usable over the bridge,
     * it just has nothing on the desktop. */
    if (!kiosk__icon_put(g_trayhl.hwnd, g_trayhl.icon, TRAY_WM_ICON, tip,
                         "headless: no tray icon (Shell_NotifyIcon failed) - this process can only be stopped from its console or Task Manager"))
        return;
    g_trayhl.tray_added = true;
}

static inline void tray__remove(void) {
    if (!g_trayhl.tray_added) return;
    kiosk__icon_drop(g_trayhl.hwnd);
    g_trayhl.tray_added = false;
}

static inline void tray__menu(int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    char    line[192];
    wchar_t wide[192];

    const bool running = g_kiosk.hooks.is_running ? g_kiosk.hooks.is_running(g_kiosk.hooks.ud)
                                                  : false;
    snprintf(line, sizeof(line), "m2-hle headless - %s - %s",
             g_kiosk.label[0] ? g_kiosk.label : "no ROM",
             running ? "running" : "paused");
    kiosk__wide(line, wide, 192);
    AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, wide);
    if (g_trayhl.note[0]) {
        kiosk__wide(g_trayhl.note, wide, 192);
        AppendMenuW(m, MF_STRING | MF_GRAYED | MF_DISABLED, 0, wide);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /*
     * NO PAUSE ITEM, and that is deliberate. A headless run is one somebody
     * else is watching -- a stream, a capture, a training loop -- and the
     * reason to open this menu at all is almost always "Restart sound board",
     * because the driver has gone quiet. A one-click pause sitting next to it
     * means the price of a mis-click is a stutter in something live. Whether
     * the board is running is still worth KNOWING, so it stays in the header
     * line and the tooltip; it is just not a button. Pause is still on the MCP
     * bridge, where it is asked for on purpose, and capture mode keeps its own
     * Run item -- that has a window, so the stutter is in front of whoever
     * caused it.
     */
    AppendMenuW(m, MF_STRING | (g_kiosk.hooks.restart_sound ? 0 : (MF_GRAYED | MF_DISABLED)),
                TRAY_ID_SNDRST, L"Restart sound board");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, TRAY_ID_EXIT, L"Exit");

    SetForegroundWindow(g_trayhl.hwnd);
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                          x, y, 0, g_trayhl.hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(g_trayhl.hwnd, WM_NULL, 0, 0);

    switch (cmd) {
        case TRAY_ID_SNDRST:
            if (g_kiosk.hooks.restart_sound) g_kiosk.hooks.restart_sound(g_kiosk.hooks.ud);
            break;
        case TRAY_ID_EXIT:
            LOG_INFO("headless: Exit chosen from the tray");
            g_trayhl.exit_requested = true;
            break;
        default: break;
    }
}

static inline LRESULT CALLBACK tray__wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_trayhl.taskbar_created && msg == g_trayhl.taskbar_created) {
        g_trayhl.tray_added = false;      /* Explorer restarted; put it back */
        tray__add();
        return 0;
    }
    switch (msg) {
        case TRAY_WM_ICON: {
            const UINT ev = LOWORD(lp);
            if (ev == WM_CONTEXTMENU || ev == WM_RBUTTONUP ||
                ev == NIN_SELECT || ev == NIN_KEYSELECT || ev == WM_LBUTTONUP)
                tray__menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            return 0;
        }
        /* The desktop asking us to go: logging off, shutting down, or somebody
         * closing the console this was started from. Treat it as Exit rather
         * than being killed halfway through a frame. */
        case WM_CLOSE:
        case WM_ENDSESSION:
        case WM_QUERYENDSESSION:
            g_trayhl.exit_requested = true;
            return msg == WM_QUERYENDSESSION ? TRUE : 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* Put the icon up. `note` is a line for the tooltip and the menu header --
 * which ports this process is answering on, so the icon identifies WHICH
 * emulator it belongs to when several are running. */
static inline bool tray_headless_start(const char *note) {
    if (g_trayhl.started) return true;
    snprintf(g_trayhl.note, sizeof(g_trayhl.note), "%s", note ? note : "");

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = tray__wndproc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"m2hle_headless_tray";
    RegisterClassExW(&wc);                 /* a second call is harmless */

    /* Top-level and never shown: see the note above about broadcasts. */
    g_trayhl.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                                    L"m2-hle", WS_OVERLAPPED,
                                    0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_trayhl.hwnd) {
        LOG_WARN("headless: could not make the tray's window (%lu)",
                 (unsigned long)GetLastError());
        return false;
    }
    g_trayhl.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_trayhl.icon) {
        int sm = GetSystemMetrics(SM_CXSMICON); if (sm < 8) sm = 16;
        g_trayhl.icon = kiosk__make_icon(sm);
    }
    tray__add();
    g_trayhl.started = true;
    if (g_trayhl.tray_added)
        LOG_INFO("headless: tray icon up - right-click it to restart the sound "
                 "board or exit");
    return true;
}

/* Drain the window's messages. Called every turn of the headless loop; it does
 * nothing at all when there is no icon. */
static inline void tray_headless_pump(void) {
    if (!g_trayhl.hwnd) return;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* The board's own frame rate, for the tooltip: without a window it is the only
 * sign of life this process has on the desktop. */
static inline void tray_headless_tick(uint64_t board_frames) {
    static uint64_t last_frames;
    static double   last_t;
    if (!g_trayhl.tray_added) return;
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    double now = (double)t.QuadPart / (double)f.QuadPart;
    if (last_t == 0.0) { last_t = now; last_frames = board_frames; return; }
    if (now - last_t < 1.0) return;
    g_trayhl.fps = (float)((double)(board_frames - last_frames) / (now - last_t));
    last_frames  = board_frames;
    last_t       = now;
    tray__update();
}

static inline void tray_headless_stop(void) {
    tray__remove();
    if (g_trayhl.icon) { DestroyIcon(g_trayhl.icon); g_trayhl.icon = NULL; }
    if (g_trayhl.hwnd) { DestroyWindow(g_trayhl.hwnd); g_trayhl.hwnd = NULL; }
    g_trayhl.started = false;
}

#else  /* not Windows: the tray and the window taming are Win32-specific. */

/* The headless tray, where there is no notification area. A POSIX run is
 * stopped with a signal, which is what it always was. */
static inline bool tray_headless_start(const char *note) { (void)note; return false; }
static inline void tray_headless_pump(void) { }
static inline void tray_headless_tick(uint64_t f) { (void)f; }
static inline void tray_headless_stop(void) { }
static inline bool tray_exit_requested(void) { return false; }


static inline void kiosk_window_ready(void) { g_kiosk.have_window = true; }
static inline void kiosk_show_window(bool show) { g_kiosk.shown = show; }
static inline void kiosk__tray_update(void) { }
static inline void kiosk_frame_tick(float dt) { (void)dt; }
static inline void kiosk_leave(void) { g_kiosk.on = false; }
static inline void kiosk_shutdown(void) { }
static inline void kiosk_enter(int w, int h, bool show) {
    (void)w; (void)h; (void)show;
    LOG_WARN("capture mode is Windows-only for now (tray icon + window taming)");
}

#endif /* _WIN32 */

/* What the tray tooltip and its menu header call the game — the loaded
 * profile's display name, since in capture mode that is the only place on the
 * desktop that says what this process is. */
static inline void kiosk_set_label(const char *s) {
    snprintf(g_kiosk.label, sizeof(g_kiosk.label), "%s", s ? s : "");
    kiosk__tray_update();
}

#endif /* KIOSK_H */
