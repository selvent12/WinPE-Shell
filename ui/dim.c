/*
 * dim.c - screen dimming for displays whose brightness cannot be set (no WMI laptop panel, no DDC/CI monitor; laptops
 * without their graphics driver in PE). Own code; idea from PExplorer/WinXShell (3df6380, brightness mask layer).
 *
 *   WinPE-ShellUI.exe dim <percent>    20..100; 100 = normal (ends the dimming)
 *
 * Two ways, the first one that works:
 *  1. gamma ramp of every display (SetDeviceGammaRamp): no extra memory, needs a display driver that supports it;
 *     Windows refuses strong dimming unless HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM GdiIcmGammaRange=256
 *     (set by the image builder)
 *  2. one click-through, semi-transparent black window per monitor. Without DWM (WinPE) the system keeps a 32-bit copy
 *     of every layered window, 4 bytes per pixel (about 8 MB of RAM for 1920x1080, 33 MB for 3840x2160), only while
 *     the screen is dimmed. (WinXShell's mask layer took 200+ MB before its fix eed58c6 - a bug, not this cost.)
 * One resident process (hidden window of class DIM_CLASS) keeps the level, re-applies it after display changes and
 * keeps the dimming windows on top; another "dim" only sends it the new level. Level 100 restores and ends it.
 */
#include "shellui.h"
#include <stdlib.h>
#include <wchar.h>

#define DIM_CLASS       L"WinPE-Shell.Dim"
#define DIM_LAYER       L"WinPE-Shell.DimLayer"
#define DIM_MIN         20
#define WM_APP_DIM      (WM_APP + 1)
#define DIM_TIMER       1
#define DIM_MAXMON      16

static struct {
    HWND ctl, layer[DIM_MAXMON];
    int nlayer, pct;
    int mode;           /* 0 none, 1 gamma, 2 layer windows */
} D;

static int Clamp(int pct) { return pct < DIM_MIN ? DIM_MIN : (pct > 100 ? 100 : pct); }

/* gamma ramp on every display attached to the desktop; pct 100 = identity. TRUE only if every display took it. */
static BOOL Gamma(int pct)
{
    DISPLAY_DEVICEW dd;
    DWORD i;
    int n = 0, ok = 0, k;
    static WORD ramp[3][256];
    for (k = 0; k < 256; k++) {
        WORD v = (WORD)(MulDiv(k * 257, pct, 100));
        ramp[0][k] = ramp[1][k] = ramp[2][k] = v;
    }
    for (i = 0; ZeroMemory(&dd, sizeof(dd)), dd.cb = sizeof(dd), EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        HDC dc;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        n++;
        dc = CreateDCW(L"DISPLAY", dd.DeviceName, NULL, NULL);
        if (!dc) continue;
        if (SetDeviceGammaRamp(dc, ramp)) ok++;
        else Log(L"dim: gamma ramp %d%% refused on %s (err=%lu)", pct, dd.DeviceName, GetLastError());
        DeleteDC(dc);
    }
    return n > 0 && ok == n;
}

static BOOL CALLBACK SumPixels(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)m; (void)dc;
    *(LONGLONG *)lp += (LONGLONG)(r->right - r->left) * (r->bottom - r->top);
    return TRUE;
}

static LONGLONG ScreenPixels(void)
{
    LONGLONG px = 0;
    EnumDisplayMonitors(NULL, NULL, SumPixels, (LPARAM)&px);
    return px;
}

/* can the screen be dimmed at all? (for the Display window: show its brightness row as "dim"); the layer windows
   always work, so yes - kept as a function for a later check */
BOOL Dim_Available(void)
{
    return TRUE;
}

/* the level of the running dimmer, 100 if none */
int Dim_Current(void)
{
    HWND h = FindWindowW(DIM_CLASS, NULL);
    int pct = h ? (int)GetWindowLongPtrW(h, GWLP_USERDATA) : 100;
    return pct >= DIM_MIN && pct <= 100 ? pct : 100;
}

/* set the level from another process: the running dimmer takes it, else a new one is started (pct < 100) */
void Dim_Set(int pct)
{
    HWND h = FindWindowW(DIM_CLASS, NULL);
    pct = Clamp(pct);
    if (h) { PostMessageW(h, WM_APP_DIM, (WPARAM)pct, 0); return; }
    if (pct < 100) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" dim %d", exe, pct);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        else Log(L"dim: start failed err=%lu", GetLastError());
    }
}

static void DestroyLayers(void)
{
    int i;
    for (i = 0; i < D.nlayer; i++) if (D.layer[i]) DestroyWindow(D.layer[i]);
    D.nlayer = 0;
}

static BOOL CALLBACK AddLayer(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    HWND w;
    (void)m; (void)dc; (void)lp;
    if (D.nlayer >= DIM_MAXMON) return FALSE;
    w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        DIM_LAYER, L"", WS_POPUP, r->left, r->top, r->right - r->left, r->bottom - r->top,
                        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!w) { Log(L"dim: layer window failed err=%lu", GetLastError()); return TRUE; }
    D.layer[D.nlayer++] = w;
    return TRUE;
}

static BOOL Layers(int pct)
{
    int i;
    BYTE alpha = (BYTE)MulDiv(100 - pct, 255, 100);
    DestroyLayers();
    EnumDisplayMonitors(NULL, NULL, AddLayer, 0);
    for (i = 0; i < D.nlayer; i++) {
        SetLayeredWindowAttributes(D.layer[i], 0, alpha, LWA_ALPHA);
        ShowWindow(D.layer[i], SW_SHOWNOACTIVATE);
    }
    return D.nlayer > 0;
}

/* apply D.pct; FALSE if this screen cannot be dimmed */
static BOOL Apply(const wchar_t *why)
{
    LONGLONG px = ScreenPixels();
    if (D.pct >= 100) {
        if (D.mode == 1) Gamma(100);
        DestroyLayers();
        D.mode = 0;
        Log(L"dim: normal brightness (%s)", why);
        return TRUE;
    }
    if (Gamma(D.pct)) {
        DestroyLayers();
        D.mode = 1;
        Log(L"dim: %d%% by gamma ramp (%s)", D.pct, why);
        return TRUE;
    }
    Gamma(100);     /* a ramp refused on one display may have been taken by the others: undo it there */
    if (Layers(D.pct)) {
        D.mode = 2;
        Log(L"dim: %d%% by %d layer window(s), %lld px = %lld KB (%s)", D.pct, D.nlayer, px, px * 4 / 1024, why);
        return TRUE;
    }
    DestroyLayers();
    D.mode = 0;
    Log(L"dim: not possible - no gamma ramp and no layer window could be created (%s)", why);
    return FALSE;
}

static LRESULT CALLBACK DimProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_APP_DIM:
        D.pct = Clamp((int)wp);
        SetWindowLongPtrW(h, GWLP_USERDATA, D.pct);
        if (!Apply(L"new level") || D.pct >= 100) DestroyWindow(h);
        return 0;
    case WM_DISPLAYCHANGE:      /* new modes reset the gamma ramp and move the monitors */
        if (!Apply(L"display change")) DestroyWindow(h);
        return 0;
    case WM_TIMER:              /* windows made topmost later (taskbar, menus) would stay above the layer */
        if (D.mode == 2) {
            int i;
            for (i = 0; i < D.nlayer; i++) SetWindowPos(D.layer[i], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    case WM_ENDSESSION:
    case WM_DESTROY:
        if (D.mode == 1) Gamma(100);
        DestroyLayers();
        D.mode = 0;
        if (m == WM_DESTROY) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int DimCommand(int argc, wchar_t **argv)
{
    WNDCLASSW wc;
    MSG msg;
    HWND h;
    int pct = argc > 2 ? Clamp(_wtoi(argv[2])) : 100;

    if ((h = FindWindowW(DIM_CLASS, NULL)) != NULL) {   /* already running: hand over the level */
        PostMessageW(h, WM_APP_DIM, (WPARAM)pct, 0);
        return 0;
    }
    if (pct >= 100) { Gamma(100); return 0; }   /* nothing running: only make sure no ramp is left over */

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DimProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = DIM_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = DIM_LAYER;
    RegisterClassW(&wc);

    /* hidden top-level window (not message-only: WM_DISPLAYCHANGE is broadcast to top-level windows only) */
    D.ctl = CreateWindowExW(WS_EX_TOOLWINDOW, DIM_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!D.ctl) { Log(L"dim: window failed err=%lu", GetLastError()); return 1; }
    D.pct = pct;
    SetWindowLongPtrW(D.ctl, GWLP_USERDATA, pct);
    if (!Apply(L"start")) {
        DestroyWindow(D.ctl);
        Message(L"The screen cannot be dimmed (no gamma ramp, no dimming layer) - see the log.");
        return 1;
    }
    SetTimer(D.ctl, DIM_TIMER, 2000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
