/*
 * MyPEUI.exe - small MyPE UI utility (docs/FEATURES.md). Own code, no third-party parts.
 * GUI subsystem, static CRT, Win32/GDI only. Sources: MyPEUI.c, display.c, dispcfg.c, brightness.c, net.c (mypeui.h).
 *
 *   MyPEUI.exe shutdown [/dryrun]       shutdown dialog (keyboard focus on Power)
 *   MyPEUI.exe restart  [/dryrun]       shutdown dialog (keyboard focus on Restart)
 *   MyPEUI.exe uri <uri>                handler for the ms-settings: and ms-availablenetworks: protocols
 *   MyPEUI.exe open <feature> [args]    display [/select N] | network | sysinfo
 *   MyPEUI.exe net on|off|toggle|status [/quiet] [/boot]   network control (net.c, spec 14)
 *   MyPEUI.exe tray-net                 network tray icon (net.c)
 *
 * Shutdown dialog: semi-opaque full-screen overlay on every monitor, two round buttons on the primary one.
 *   Restart -> wpeutil Reboot, Power -> wpeutil Shutdown. Esc, click outside the buttons or a display change = cancel.
 *   /dryrun: the wpeutil call is replaced by a "DRYRUN ..." line in the log (VM tests).
 *
 * Features are switched in X:\MyPE\config\mypeui.ini   [features]  <name>=<value>:
 *   enabled          the built-in window (display = spec 5, display.c; ms-settings:display / ms-settings:screenrotation;
 *                    network = spec 14, net.c; ms-availablenetworks:, ms-settings:network*)
 *   (empty)          short "not available yet" message (system info = spec 13 is not built yet; network if the image has none)
 *   <command line>   that program; the URI, if any, is appended as one argument
 *
 * Log: X:\MyPE\logs\mypeui.log
 */
#include "mypeui.h"
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#define INI       MYPE_INI
#define LOGDIR    MYPE_LOGDIR
#define LOGFILE   MYPE_LOGFILE
#define OV_CLASS  L"MyPE.ShutdownOverlay"
#define OV_MUTEX  L"Local\\MyPE.ShutdownOverlay"
#define MSG_TITLE MYPE_MSG_TITLE
#define MAXOV     16
#define BG_ALPHA  200          /* overlay opacity 0..255 */

static int g_argc;
static wchar_t **g_argv;

void Log(const wchar_t *fmt, ...)
{
    wchar_t msg[1024], line[1200];
    char utf8[3600];
    SYSTEMTIME t;
    va_list ap;
    HANDLE h;
    DWORD n;
    int len;

    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    GetLocalTime(&t);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u MyPEUI[%lu] %s\r\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), msg);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 1) return;
    CreateDirectoryW(L"X:\\MyPE", NULL);
    CreateDirectoryW(LOGDIR, NULL);
    h = CreateFileW(LOGFILE, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, utf8, (DWORD)(len - 1), &n, NULL);
    CloseHandle(h);
}

void Message(const wchar_t *fmt, ...)
{
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    MessageBoxW(NULL, text, MSG_TITLE, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

static void SetDpiAware(void)
{
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetCtx f = u ? (SetCtx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!f || !f((HANDLE)(INT_PTR)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */))
        SetProcessDPIAware();
}

UINT WindowDpi(HWND h)
{
    typedef UINT (WINAPI *GetDpi)(HWND);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    GetDpi f = u ? (GetDpi)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    UINT d = f ? f(h) : 0;
    if (!d) { HDC dc = GetDC(NULL); d = (UINT)GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    return d ? d : 96;
}

/* ======================================================================================================== */
/* features: built-in (display), pluggable through mypeui.ini, otherwise a placeholder message               */
/* ======================================================================================================== */

/* Runs the command configured for the feature; returns FALSE if none is configured.
   *builtin = TRUE if the value is "enabled" (built-in window; only display has one). */
static BOOL RunPlugged(const wchar_t *feature, const wchar_t *uri, BOOL *builtin)
{
    wchar_t cmd[2048], line[2600];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    *builtin = FALSE;
    if (!GetPrivateProfileStringW(L"features", feature, L"", cmd, _countof(cmd), INI) || !cmd[0]) return FALSE;
    if (!_wcsicmp(cmd, L"enabled")) {
        if (!_wcsicmp(feature, L"display") || !_wcsicmp(feature, L"network")) { *builtin = TRUE; return TRUE; }
        Log(L"feature %s: 'enabled' but no built-in window in this MyPEUI version", feature);
        return FALSE;
    }
    if (uri && uri[0]) _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s \"%s\"", cmd, uri);
    else wcsncpy_s(line, _countof(line), cmd, _TRUNCATE);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Log(L"feature %s: start FAILED err=%lu: %s", feature, GetLastError(), line);
        Message(L"MyPE could not start the handler for '%s'.\n\n%s", feature, line);
        return TRUE;
    }
    Log(L"feature %s -> pid %lu: %s", feature, pi.dwProcessId, line);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static int OpenFeature(const wchar_t *feature, const wchar_t *uri)
{
    BOOL builtin;
    if (RunPlugged(feature, uri, &builtin)) {
        if (!builtin) return 0;
        Log(L"feature %s: built-in window (uri=%s)", feature, uri ? uri : L"");
        if (!_wcsicmp(feature, L"network")) return NetWindow(uri);
        return DisplayWindow(uri, g_argc, g_argv);
    }
    Log(L"feature %s: placeholder (uri=%s)", feature, uri ? uri : L"");
    if (!_wcsicmp(feature, L"display"))
        Message(L"Display settings are not available yet in MyPE.\n\n(%s)", uri && uri[0] ? uri : L"display");
    else if (!_wcsicmp(feature, L"network"))
        Message(L"Network settings are not available in this MyPE build.\nNetworking is not included in the image.\n\n(%s)", uri && uri[0] ? uri : L"network");
    else if (!_wcsicmp(feature, L"sysinfo"))
        Message(L"System properties are not available yet in MyPE.");
    else
        Message(L"'%s' is not available in MyPE.", feature);
    return 0;
}

/* ms-settings:<page>[?query] and ms-availablenetworks: -> feature name, or NULL (= not available in MyPE) */
static const wchar_t *UriFeature(const wchar_t *uri)
{
    wchar_t u[512], *q, *page;
    size_t i;
    wcsncpy_s(u, _countof(u), uri, _TRUNCATE);
    for (i = 0; u[i]; i++) u[i] = (wchar_t)towlower(u[i]);
    if (!wcsncmp(u, L"ms-availablenetworks:", 21)) return L"network";
    if (wcsncmp(u, L"ms-settings:", 12)) return NULL;
    page = u + 12;
    while (*page == L'/') page++;
    if ((q = wcschr(page, L'?')) != NULL) *q = 0;
    if (!wcsncmp(page, L"network", 7)) return L"network";
    if (!wcscmp(page, L"display") || !wcsncmp(page, L"display-", 8) || !wcscmp(page, L"screenrotation")) return L"display";
    return NULL;
}

static int HandleUri(const wchar_t *uri)
{
    const wchar_t *f = UriFeature(uri);
    Log(L"uri '%s' -> %s", uri, f ? f : L"(not available)");
    if (f) return OpenFeature(f, uri);
    Message(L"'%s' is not available in MyPE.", uri);
    return 0;
}

/* ======================================================================================================== */
/* shutdown dialog                                                                                           */
/* ======================================================================================================== */

typedef struct { HWND hwnd; RECT rc; BOOL primary; HDC mem; HBITMAP bmp; HGDIOBJ old; DWORD *bits; } Overlay;
typedef struct { double cx, cy, r; const wchar_t *label; const wchar_t *verb; } Button;   /* centre in window coords */

static Overlay g_ov[MAXOV];
static int g_nov;
static Button g_btn[2] = { {0, 0, 0, L"Restart", L"Reboot"}, {0, 0, 0, L"Shut down", L"Shutdown"} };
static int g_focus = 1, g_hover = -1, g_pressed = -1, g_downOutside = 0, g_done = 0;
static BOOL g_dryrun = FALSE;
static double g_s = 1.0;      /* DPI scale of the primary monitor */
static HFONT g_font, g_fontSmall;

static double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* source-over blend of a colour with coverage cov into a premultiplied BGRA pixel */
static void Blend(DWORD *px, int r, int g, int b, double cov)
{
    DWORD d = *px;
    int sa, inv, da, dr, dg, db;
    cov = Clamp01(cov);
    if (cov <= 0.0) return;
    sa = (int)(cov * 255.0 + 0.5); inv = 255 - sa;
    da = (int)(d >> 24); dr = (int)((d >> 16) & 255); dg = (int)((d >> 8) & 255); db = (int)(d & 255);
    da = sa + da * inv / 255;
    dr = r * sa / 255 + dr * inv / 255;
    dg = g * sa / 255 + dg * inv / 255;
    db = b * sa / 255 + db * inv / 255;
    *px = ((DWORD)da << 24) | ((DWORD)dr << 16) | ((DWORD)dg << 8) | (DWORD)db;
}

/* signed distance of p to the edge a->b, positive on the left side for a counter-clockwise triangle */
static double EdgeDist(double px, double py, double ax, double ay, double bx, double by)
{
    double ex = bx - ax, ey = by - ay, len = sqrt(ex * ex + ey * ey);
    return len > 0 ? ((px - ax) * ey - (py - ay) * ex) / len : 0;
}

static double TriangleCov(double px, double py, const double t[6])
{
    double d1, d2, d3, m, area = (t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]);
    d1 = EdgeDist(px, py, t[0], t[1], t[2], t[3]);
    d2 = EdgeDist(px, py, t[2], t[3], t[4], t[5]);
    d3 = EdgeDist(px, py, t[4], t[5], t[0], t[1]);
    if (area > 0) { d1 = -d1; d2 = -d2; d3 = -d3; }
    m = d1 < d2 ? d1 : d2; m = m < d3 ? m : d3;
    return Clamp01(m + 0.5);
}

/* coverage of the white icon (0 = restart arrow, 1 = power symbol) at pixel centre (px, py) */
static double IconCov(int which, const Button *b, double px, double py)
{
    const double PI = 3.14159265358979;
    double dx = px - b->cx, dy = py - b->cy, d = sqrt(dx * dx + dy * dy);
    double ri = 0.40 * b->r, t = 0.085 * b->r, a = atan2(dx, -dy) * 180.0 / PI;   /* a: 0 = up, clockwise positive */
    double ring = Clamp01(t / 2 - fabs(d - ri) + 0.5), cov = 0;
    if (which == 1) {
        double y0 = b->cy - ri - t * 0.15, y1 = b->cy - 0.08 * b->r, dist;
        if (fabs(a) >= 38.0) cov = ring;
        if (py < y0) dist = sqrt(dx * dx + (py - y0) * (py - y0));
        else if (py > y1) dist = sqrt(dx * dx + (py - y1) * (py - y1));
        else dist = fabs(dx);
        cov = max(cov, Clamp01(t / 2 - dist + 0.5));
    } else {
        double e = 18.0 * PI / 180.0, ex, ey, tx, ty, nx, ny, w = 0.17 * b->r, h = 0.21 * b->r, tri[6];
        if (!(a >= 18.0 && a <= 78.0)) cov = ring;
        ex = b->cx + ri * sin(e); ey = b->cy - ri * cos(e);   /* arc end (clockwise), arrow head points along it */
        tx = cos(e); ty = sin(e); nx = sin(e); ny = -cos(e);
        tri[0] = ex + nx * w; tri[1] = ey + ny * w;
        tri[2] = ex - nx * w; tri[3] = ey - ny * w;
        tri[4] = ex + tx * h; tri[5] = ey + ty * h;
        cov = max(cov, TriangleCov(px, py, tri));
    }
    return cov;
}

/* text through a black/white grey-scale bitmap, used as coverage (GDI text on a 32-bit DIB leaves alpha at 0) */
static void DrawLabel(Overlay *o, RECT r, const wchar_t *text, HFONT font, double opacity)
{
    int W = o->rc.right - o->rc.left, H = o->rc.bottom - o->rc.top, w = r.right - r.left, h = r.bottom - r.top, x, y;
    BITMAPINFO bi;
    DWORD *tb = NULL;
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ ob, of;
    RECT lr;
    if (w <= 0 || h <= 0) return;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&tb, NULL, 0);
    if (!bmp || !tb) { DeleteDC(dc); return; }
    ob = SelectObject(dc, bmp); of = SelectObject(dc, font);
    ZeroMemory(tb, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    SetRect(&lr, 0, 0, w, h);
    DrawTextW(dc, text, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    for (y = 0; y < h; y++) {
        int Y = r.top + y;
        if (Y < 0 || Y >= H) continue;
        for (x = 0; x < w; x++) {
            int X = r.left + x;
            DWORD p = tb[y * w + x];
            int c = (int)max(max((p >> 16) & 255, (p >> 8) & 255), p & 255);
            if (X < 0 || X >= W || !c) continue;
            Blend(&o->bits[Y * W + X], 255, 255, 255, opacity * c / 255.0);
        }
    }
    SelectObject(dc, of); SelectObject(dc, ob);
    DeleteObject(bmp); DeleteDC(dc);
}

static void Layout(Overlay *p)
{
    int W = p->rc.right - p->rc.left, H = p->rc.bottom - p->rc.top;
    double r = 56.0 * g_s, gap = 2 * r + 80.0 * g_s;
    g_btn[0].r = g_btn[1].r = r;
    g_btn[0].cx = W / 2.0 - gap / 2; g_btn[1].cx = W / 2.0 + gap / 2;
    g_btn[0].cy = g_btn[1].cy = H / 2.0 - 24.0 * g_s;
}

static void Render(Overlay *o)
{
    int W = o->rc.right - o->rc.left, H = o->rc.bottom - o->rc.top, i, x, y;
    DWORD bg = (DWORD)BG_ALPHA << 24;   /* premultiplied black */
    POINT src = {0, 0}, pos = {o->rc.left, o->rc.top};
    SIZE sz = {W, H};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    size_t n = (size_t)W * H, k;
    for (k = 0; k < n; k++) o->bits[k] = bg;
    if (o->primary) {
        for (i = 0; i < 2; i++) {
            Button *b = &g_btn[i];
            double pad = 8.0 * g_s, rr = b->r + pad;
            int x0 = (int)(b->cx - rr), x1 = (int)(b->cx + rr) + 1, y0 = (int)(b->cy - rr), y1 = (int)(b->cy + rr) + 1;
            int shade = (g_pressed == i && g_hover == i) ? 0x4A : (g_hover == i ? 0x86 : 0x66);
            RECT lr;
            for (y = max(y0, 0); y < min(y1, H); y++) {
                for (x = max(x0, 0); x < min(x1, W); x++) {
                    double px = x + 0.5, py = y + 0.5, dx = px - b->cx, dy = py - b->cy, d = sqrt(dx * dx + dy * dy);
                    DWORD *p = &o->bits[y * W + x];
                    Blend(p, shade, shade, shade, b->r - d + 0.5);
                    if (d < b->r + 1) Blend(p, 255, 255, 255, IconCov(i, b, px, py));
                    if (g_focus == i) Blend(p, 255, 255, 255, 0.85 * Clamp01(1.4 * g_s - fabs(d - (b->r + 5.0 * g_s)) + 0.5));
                }
            }
            lr.left = (LONG)(b->cx - b->r - 40 * g_s); lr.right = (LONG)(b->cx + b->r + 40 * g_s);
            lr.top = (LONG)(b->cy + b->r + 14 * g_s); lr.bottom = lr.top + (LONG)(34 * g_s);
            DrawLabel(o, lr, b->label, g_font, 1.0);
        }
        {
            RECT hr;
            hr.left = 0; hr.right = W;
            hr.top = (LONG)(g_btn[0].cy + g_btn[0].r + 60 * g_s); hr.bottom = hr.top + (LONG)(28 * g_s);
            DrawLabel(o, hr, L"Esc = cancel", g_fontSmall, 0.6);
        }
    }
    UpdateLayeredWindow(o->hwnd, NULL, &pos, &sz, o->mem, &src, 0, &bf, ULW_ALPHA);
}

static Overlay *Primary(void)
{
    int i;
    for (i = 0; i < g_nov; i++) if (g_ov[i].primary) return &g_ov[i];
    return g_nov ? &g_ov[0] : NULL;
}

static int HitTest(HWND h, LPARAM lp)
{
    Overlay *p = Primary();
    double x = (short)LOWORD(lp) + 0.5, y = (short)HIWORD(lp) + 0.5;
    int i;
    if (!p || p->hwnd != h) return -1;
    for (i = 0; i < 2; i++) {
        double dx = x - g_btn[i].cx, dy = y - g_btn[i].cy;
        if (dx * dx + dy * dy <= g_btn[i].r * g_btn[i].r) return i;
    }
    return -1;
}

static void CloseAll(const wchar_t *why)
{
    int i;
    if (g_done) return;
    g_done = 1;
    Log(L"overlay closed: %s", why);
    for (i = 0; i < g_nov; i++) if (g_ov[i].hwnd) ShowWindow(g_ov[i].hwnd, SW_HIDE);
    PostQuitMessage(0);
}

static void DoAction(int i)
{
    wchar_t exe[MAX_PATH], line[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    int j;

    if (g_done) return;
    g_done = 1;
    for (j = 0; j < g_nov; j++) if (g_ov[j].hwnd) ShowWindow(g_ov[j].hwnd, SW_HIDE);
    if (g_dryrun) {
        Log(L"DRYRUN wpeutil %s (button %s)", g_btn[i].verb, g_btn[i].label);
        PostQuitMessage(0);
        return;
    }
    GetSystemDirectoryW(exe, MAX_PATH);
    wcscat_s(exe, _countof(exe), L"\\wpeutil.exe");
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"\"%s\" %s", exe, g_btn[i].verb);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    Log(L"action %s: %s", g_btn[i].label, line);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log(L"wpeutil start FAILED err=%lu", GetLastError());
        Message(L"MyPE could not run:\n%s\n\nError %lu.", line, GetLastError());
    } else {
        CloseHandle(pi.hThread);
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hProcess);
        Log(L"wpeutil %s rc=%lu", g_btn[i].verb, rc);
        if (rc) Message(L"wpeutil %s failed (exit code %lu).", g_btn[i].verb, rc);
    }
    PostQuitMessage(0);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Overlay *p = Primary();
    switch (m) {
    case WM_MOUSEMOVE: {
        int hit = HitTest(h, lp);
        if (hit != g_hover) { g_hover = hit; if (p) Render(p); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int hit = HitTest(h, lp);
        g_pressed = hit; g_downOutside = (hit < 0);
        if (hit >= 0) { g_focus = hit; SetCapture(h); if (p) Render(p); }
        return 0;
    }
    case WM_LBUTTONUP: {
        int hit = HitTest(h, lp), was = g_pressed;
        if (GetCapture() == h) ReleaseCapture();
        g_pressed = -1;
        if (was >= 0 && hit == was) DoAction(was);
        else if (was < 0 && g_downOutside) CloseAll(L"click outside the buttons");
        else if (p) Render(p);
        g_downOutside = 0;
        return 0;
    }
    case WM_RBUTTONUP:
        if (HitTest(h, lp) < 0) CloseAll(L"right click outside the buttons");
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE: CloseAll(L"Esc"); break;
        case VK_LEFT: case VK_RIGHT: case VK_TAB: g_focus = 1 - g_focus; if (p) Render(p); break;
        case VK_RETURN: case VK_SPACE: DoAction(g_focus); break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        CloseAll(L"display change");
        return 0;
    case WM_CLOSE:
        CloseAll(L"WM_CLOSE");
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static BOOL CALLBACK MonitorCb(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
    MONITORINFO mi;
    (void)dc; (void)r; (void)lp;
    if (g_nov >= MAXOV) return FALSE;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    g_ov[g_nov].rc = mi.rcMonitor;
    g_ov[g_nov].primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    g_nov++;
    return TRUE;
}

void ForceForeground(HWND h)
{
    HWND fg;
    DWORD ft, me = GetCurrentThreadId();
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) { SetFocus(h); return; }
    /* started by a process that is not in the foreground (e.g. a script): attach to the foreground input queue */
    fg = GetForegroundWindow();
    ft = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (ft && ft != me && AttachThreadInput(me, ft, TRUE)) {
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, ft, FALSE);
    }
    SetFocus(h);
}

static BOOL CreateBuffer(Overlay *o)
{
    BITMAPINFO bi;
    int W = o->rc.right - o->rc.left, H = o->rc.bottom - o->rc.top;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    o->mem = CreateCompatibleDC(NULL);
    o->bmp = CreateDIBSection(o->mem, &bi, DIB_RGB_COLORS, (void **)&o->bits, NULL, 0);
    if (!o->bmp || !o->bits) return FALSE;
    o->old = SelectObject(o->mem, o->bmp);
    return TRUE;
}

static int ShutdownDialog(BOOL focusRestart)
{
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    HANDLE mutex;
    Overlay *p;
    MSG msg;
    int i;

    mutex = CreateMutexW(NULL, TRUE, OV_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(OV_CLASS, NULL);
        Log(L"overlay already open (hwnd %p) - brought to front", (void *)other);
        if (other) ForceForeground(other);
        CloseHandle(mutex);
        return 0;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = OverlayProc; wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.lpszClassName = OV_CLASS;
    if (!RegisterClassExW(&wc)) { Log(L"RegisterClass failed err=%lu", GetLastError()); return 1; }

    EnumDisplayMonitors(NULL, NULL, MonitorCb, 0);
    if (!g_nov) {   /* no monitor info: whole primary screen */
        SetRect(&g_ov[0].rc, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        g_ov[0].primary = TRUE; g_nov = 1;
    }
    g_focus = focusRestart ? 0 : 1;

    for (i = 0; i < g_nov; i++) {
        Overlay *o = &g_ov[i];
        o->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OV_CLASS,
                                  o->primary ? L"MyPE Shutdown" : L"", WS_POPUP,
                                  o->rc.left, o->rc.top, o->rc.right - o->rc.left, o->rc.bottom - o->rc.top,
                                  NULL, NULL, hi, NULL);
        if (!o->hwnd || !CreateBuffer(o)) { Log(L"overlay window %d failed err=%lu", i, GetLastError()); return 1; }
    }
    p = Primary();
    g_s = WindowDpi(p->hwnd) / 96.0;
    g_font = CreateFontW(-(int)(15 * g_s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(-(int)(12 * g_s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    Layout(p);
    for (i = 0; i < g_nov; i++) { Render(&g_ov[i]); ShowWindow(g_ov[i].hwnd, SW_SHOWNA); }
    ForceForeground(p->hwnd);
    Log(L"overlay shown%s: monitors=%d primary=%ld,%ld,%ld,%ld dpi-scale=%.2f restart=%ld,%ld,%ld power=%ld,%ld,%ld foreground=%d",
        g_dryrun ? L" (dryrun)" : L"", g_nov, p->rc.left, p->rc.top, p->rc.right - p->rc.left, p->rc.bottom - p->rc.top, g_s,
        p->rc.left + (long)g_btn[0].cx, p->rc.top + (long)g_btn[0].cy, (long)g_btn[0].r,
        p->rc.left + (long)g_btn[1].cx, p->rc.top + (long)g_btn[1].cy, (long)g_btn[1].r,
        GetForegroundWindow() == p->hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    for (i = 0; i < g_nov; i++) {
        if (g_ov[i].hwnd) DestroyWindow(g_ov[i].hwnd);
        if (g_ov[i].mem) { SelectObject(g_ov[i].mem, g_ov[i].old); DeleteDC(g_ov[i].mem); }
        if (g_ov[i].bmp) DeleteObject(g_ov[i].bmp);
    }
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}

/* ======================================================================================================== */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    int argc = 0, i, rc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *cmd;
    (void)hInst; (void)hPrev; (void)cmdLine; (void)nShow;

    SetDpiAware();
    g_argc = argc; g_argv = argv;
    if (!argv || argc < 2) {
        Message(L"MyPEUI.exe shutdown|restart [/dryrun]\nMyPEUI.exe uri <uri>\nMyPEUI.exe open display [/select N] | network | sysinfo\nMyPEUI.exe net on|off|toggle|status [/quiet]\nMyPEUI.exe tray-net");
        if (argv) LocalFree(argv);
        return 2;
    }
    for (i = 2; i < argc; i++) if (!_wcsicmp(argv[i], L"/dryrun") || !_wcsicmp(argv[i], L"-dryrun")) g_dryrun = TRUE;
    cmd = argv[1];
    Log(L"start: %s", GetCommandLineW());
    if (!_wcsicmp(cmd, L"shutdown")) rc = ShutdownDialog(FALSE);
    else if (!_wcsicmp(cmd, L"restart")) rc = ShutdownDialog(TRUE);
    else if (!_wcsicmp(cmd, L"uri")) rc = HandleUri(argc > 2 ? argv[2] : L"");
    else if (!_wcsicmp(cmd, L"open")) rc = OpenFeature(argc > 2 ? argv[2] : L"", NULL);
    else if (!_wcsicmp(cmd, L"net")) rc = NetCommand(argc, argv);
    else if (!_wcsicmp(cmd, L"tray-net")) rc = NetTray();
    else if (!_wcsnicmp(cmd, L"ms-settings:", 12) || !_wcsnicmp(cmd, L"ms-availablenetworks:", 21)) rc = HandleUri(cmd);
    else { Log(L"unknown command '%s'", cmd); Message(L"MyPEUI: unknown command '%s'.", cmd); rc = 2; }
    LocalFree(argv);
    return rc;
}
