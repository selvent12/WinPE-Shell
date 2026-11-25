/*
 * vol.c - MyPE volume tray icon (docs/FEATURES.md). Own code, Win32/GDI + Core Audio (MMDevice API) in C.
 *
 *   MyPEUI.exe tray-vol      tray icon of the default playback device (started at boot, single instance)
 *
 * - icon: speaker with 0-2 sound waves by level, a cross when muted or when there is no playback device;
 *   tooltip: device name + level;
 * - left click: small volume window above the icon (slider, mute button, "Mixer"); closes when it loses the focus;
 * - right click: menu (Mute, Volume mixer = SndVol.exe, Sound settings = mmsys.cpl);
 * - the state is read every 500 ms (cheap; also catches changes made by other programs and a new default device);
 *   a device that disappears (AUDCLNT_E_DEVICE_INVALIDATED) is re-acquired.
 * Test hooks (WM_COMMAND to the hidden window, class MyPE.TrayVol): 20 mute toggle, 21 volume window, 22 menu,
 * 23 log the state. Log: "tray-vol: ...".
 */
#define COBJMACROS
#include "mypeui.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

#define TV_CLASS   L"MyPE.TrayVol"
#define TV_FLYOUT  L"MyPE.VolFlyout"
#define TV_MUTEX   L"Local\\MyPE.TrayVol"
#define WM_TVCB    (WM_APP + 0x41)
enum { IDC_V_SLIDER = 501, IDC_V_MUTE, IDC_V_MIXER, IDC_V_LABEL };

/* GUIDs defined here (C: no __uuidof) */
static const CLSID s_clsidEnum = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID s_iidEnum = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID s_iidVol = { 0x5CDF2C82, 0x841E, 0x4546, { 0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A } };
static const PROPERTYKEY s_pkeyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

static struct {
    HWND hwnd, fly, slider, muteBtn, label;
    UINT taskbarCreated;
    BOOL added;
    DWORD lastAddTry, lastAcquire;
    IMMDeviceEnumerator *en;
    IAudioEndpointVolume *vol;
    wchar_t devId[256], devName[128];
    int level;                 /* 0..100, -1 = no device */
    BOOL muted;
    int iconLevel, iconMuted;  /* what the current icon shows */
    HICON icon;
    HFONT font;
    double s;
    DWORD flyClosedAt;
} V;

/* ---------------------------------------------------------------- icon (anti-aliased, drawn per pixel) */

static double Cl(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void Px(DWORD *bits, int sz, int x, int y, DWORD rgb, double a)
{
    DWORD *p = &bits[y * sz + x];
    double ea = ((*p >> 24) & 0xFF) / 255.0, na = a + ea * (1 - a);
    int c;
    DWORD out = 0;
    if (a <= 0) return;
    for (c = 0; c < 3; c++) {
        double src = ((rgb >> (c * 8)) & 0xFF), dst = ((*p >> (c * 8)) & 0xFF);
        double v = na > 0 ? (src * a + dst * ea * (1 - a)) / na : 0;
        out |= ((DWORD)(v + 0.5) & 0xFF) << (c * 8);
    }
    *p = out | ((DWORD)(na * 255 + 0.5) << 24);
}

/* coverage of a convex polygon at a pixel (4x4 supersampling) */
static double PolyCover(const double *xs, const double *ys, int n, int px, int py)
{
    int sx, sy, hit = 0, i;
    for (sy = 0; sy < 4; sy++) for (sx = 0; sx < 4; sx++) {
        double x = px + (sx + 0.5) / 4, y = py + (sy + 0.5) / 4;
        int inside = 1;
        for (i = 0; i < n && inside; i++) {
            int j = (i + 1) % n;
            if ((xs[j] - xs[i]) * (y - ys[i]) - (ys[j] - ys[i]) * (x - xs[i]) < 0) inside = 0;
        }
        hit += inside;
    }
    return hit / 16.0;
}

static double SegDist(double px, double py, double ax, double ay, double bx, double by)
{
    double ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey, u = l2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / l2 : 0;
    u = Cl(u);
    return sqrt((ax + u * ex - px) * (ax + u * ex - px) + (ay + u * ey - py) * (ay + u * ey - py));
}

static HICON MakeVolIcon(int level, BOOL muted, int sz)
{
    BITMAPV5HEADER bh;
    DWORD *bits = NULL;
    HDC dc;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic;
    int x, y, w;
    double s = sz / 16.0;
    DWORD white = 0xFFFFFF, gray = 0x9AA0A6;
    BOOL off = muted || level < 0;
    /* speaker: body + cone (clockwise for PolyCover) */
    double bx[4] = { 1.5 * s, 4.6 * s, 4.6 * s, 1.5 * s }, by[4] = { 5.8 * s, 5.8 * s, 10.2 * s, 10.2 * s };
    double cx[4] = { 4.2 * s, 8.6 * s, 8.6 * s, 4.2 * s }, cy[4] = { 5.8 * s, 2.0 * s, 14.0 * s, 10.2 * s };

    ZeroMemory(&bh, sizeof(bh));
    bh.bV5Size = sizeof(bh); bh.bV5Width = sz; bh.bV5Height = -sz; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS; bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    ZeroMemory(bits, (size_t)sz * sz * 4);
    for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
        double a = PolyCover(bx, by, 4, x, y), b = PolyCover(cx, cy, 4, x, y);
        Px(bits, sz, x, y, off ? gray : white, a > b ? a : b);
    }
    if (off) {   /* cross on the right */
        double t = 0.85 * s;
        for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
            double d1 = SegDist(x + 0.5, y + 0.5, 10.4 * s, 5.4 * s, 14.6 * s, 10.6 * s);
            double d2 = SegDist(x + 0.5, y + 0.5, 10.4 * s, 10.6 * s, 14.6 * s, 5.4 * s);
            double d = d1 < d2 ? d1 : d2;
            Px(bits, sz, x, y, level < 0 ? 0x9AA0A6 : 0xE04848, Cl(t - d + 0.5));
        }
    } else {     /* sound waves: arcs around the cone's mouth */
        int waves = level == 0 ? 0 : (level <= 50 ? 1 : 2);
        for (w = 0; w < waves; w++) {
            double r = (w == 0 ? 3.0 : 5.6) * s, th = 0.75 * s, ox = 8.6 * s, oy = 8.0 * s;
            for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
                double dx = x + 0.5 - ox, dy = y + 0.5 - oy, d = sqrt(dx * dx + dy * dy);
                double ang = atan2(fabs(dy), dx);   /* 0 = straight right */
                if (dx <= 0 || ang > 0.95) continue;
                Px(bits, sz, x, y, white, Cl(th - fabs(d - r) + 0.5));
            }
        }
    }
    mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return ic;
}

/* ---------------------------------------------------------------- Core Audio */

static void ReleaseDevice(void)
{
    if (V.vol) { IAudioEndpointVolume_Release(V.vol); V.vol = NULL; }
    V.devId[0] = 0; V.devName[0] = 0;
}

static BOOL Acquire(void)
{
    IMMDevice *dev = NULL;
    IPropertyStore *ps = NULL;
    LPWSTR id = NULL;
    HRESULT hr;
    ReleaseDevice();
    V.lastAcquire = GetTickCount();
    if (!V.en) {
        hr = CoCreateInstance(&s_clsidEnum, NULL, CLSCTX_ALL, &s_iidEnum, (void **)&V.en);
        if (FAILED(hr)) { Log(L"tray-vol: MMDeviceEnumerator not available hr=0x%08lX (audio services?)", (unsigned long)hr); V.en = NULL; return FALSE; }
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(V.en, eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) return FALSE;
    hr = IMMDevice_Activate(dev, &s_iidVol, CLSCTX_ALL, NULL, (void **)&V.vol);
    if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) { wcsncpy_s(V.devId, _countof(V.devId), id, _TRUNCATE); CoTaskMemFree(id); }
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &s_pkeyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            wcsncpy_s(V.devName, _countof(V.devName), pv.pwszVal, _TRUNCATE);
        PropVariantClear(&pv);
        IPropertyStore_Release(ps);
    }
    IMMDevice_Release(dev);
    if (FAILED(hr) || !V.vol) { V.vol = NULL; Log(L"tray-vol: endpoint volume not available hr=0x%08lX", (unsigned long)hr); return FALSE; }
    Log(L"tray-vol: device '%s'", V.devName[0] ? V.devName : L"(no name)");
    return TRUE;
}

/* the default device may have changed (e.g. headphones plugged in): compare its id */
static BOOL DefaultChanged(void)
{
    IMMDevice *dev = NULL;
    LPWSTR id = NULL;
    BOOL changed = FALSE;
    if (!V.en || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(V.en, eRender, eConsole, &dev)) || !dev) return V.vol != NULL;
    if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && id) { changed = wcscmp(id, V.devId) != 0; CoTaskMemFree(id); }
    IMMDevice_Release(dev);
    return changed;
}

static void ReadState(void)
{
    float f = 0;
    BOOL m = FALSE;
    static DWORD lastCheck;
    if (V.vol && GetTickCount() - lastCheck > 3000) { lastCheck = GetTickCount(); if (DefaultChanged()) { Log(L"tray-vol: default device changed"); Acquire(); } }
    if (!V.vol && GetTickCount() - V.lastAcquire > 3000) Acquire();
    if (!V.vol) { V.level = -1; V.muted = FALSE; return; }
    if (FAILED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(V.vol, &f)) || FAILED(IAudioEndpointVolume_GetMute(V.vol, &m))) {
        Log(L"tray-vol: device lost - re-acquiring");
        if (!Acquire()) { V.level = -1; return; }
        IAudioEndpointVolume_GetMasterVolumeLevelScalar(V.vol, &f);
        IAudioEndpointVolume_GetMute(V.vol, &m);
    }
    V.level = (int)(f * 100 + 0.5);
    V.muted = m;
}

static void SetLevel(int pct)
{
    if (!V.vol) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    IAudioEndpointVolume_SetMasterVolumeLevelScalar(V.vol, pct / 100.0f, NULL);
    if (pct > 0 && V.muted) IAudioEndpointVolume_SetMute(V.vol, FALSE, NULL);
}

static void ToggleMute(void)
{
    if (!V.vol) return;
    IAudioEndpointVolume_SetMute(V.vol, !V.muted, NULL);
    Log(L"tray-vol: mute %s", !V.muted ? L"on" : L"off");
}

/* ---------------------------------------------------------------- tray icon */

static void TrayUpdate(BOOL force)
{
    NOTIFYICONDATAW nd;
    int oldLevel = V.level;
    BOOL oldMuted = V.muted;
    ReadState();
    if (!force && V.added && oldLevel == V.level && oldMuted == V.muted) return;
    {   /* the icon only changes with the number of waves / mute / no device */
        int shown = V.level < 0 ? -1 : (V.level == 0 ? 0 : (V.level <= 50 ? 1 : 2));
        if (!V.icon || V.iconLevel != shown || V.iconMuted != (int)V.muted) {
            HICON old = V.icon;
            V.icon = MakeVolIcon(V.level < 0 ? -1 : (shown == 0 ? 0 : (shown == 1 ? 40 : 80)), V.muted, GetSystemMetrics(SM_CXSMICON));
            V.iconLevel = shown; V.iconMuted = V.muted;
            if (old) DestroyIcon(old);
        }
    }
    ZeroMemory(&nd, sizeof(nd));
    nd.cbSize = sizeof(nd); nd.hWnd = V.hwnd; nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nd.uCallbackMessage = WM_TVCB; nd.hIcon = V.icon;
    if (V.level < 0) wcscpy_s(nd.szTip, _countof(nd.szTip), L"No audio output device");
    else _snwprintf_s(nd.szTip, _countof(nd.szTip), _TRUNCATE, L"%s: %d%%%s", V.devName[0] ? V.devName : L"Speakers", V.level, V.muted ? L" (muted)" : L"");
    if (V.fly && IsWindowVisible(V.fly)) {   /* keep the open volume window in step */
        wchar_t t[32];
        if (GetFocus() != V.slider) SendMessageW(V.slider, TBM_SETPOS, TRUE, V.level < 0 ? 0 : V.level);
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%", V.level < 0 ? 0 : V.level);
        SetWindowTextW(V.label, t);
        SetWindowTextW(V.muteBtn, V.muted ? L"Unmute" : L"Mute");
    }
    if (V.added && Shell_NotifyIconW(NIM_MODIFY, &nd)) return;
    if (Shell_NotifyIconW(NIM_ADD, &nd)) { if (!V.added) Log(L"tray-vol: icon added (%s %d%%%s)", V.devName, V.level, V.muted ? L" muted" : L""); V.added = TRUE; }
    else { if (V.added || GetTickCount() - V.lastAddTry > 30000) Log(L"tray-vol: Shell_NotifyIcon NIM_ADD failed err=%lu (no tray yet?) - retrying", GetLastError()); V.added = FALSE; V.lastAddTry = GetTickCount(); }
}

static void Spawn(const wchar_t *cmdline)
{
    wchar_t line[MAX_PATH * 2];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wcsncpy_s(line, _countof(line), cmdline, _TRUNCATE);
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    if (CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); Log(L"tray-vol: started %s", cmdline); }
    else Log(L"tray-vol: start FAILED err=%lu: %s", GetLastError(), cmdline);
}

static void OpenMixer(void) { Spawn(L"X:\\Windows\\System32\\SndVol.exe"); }
static void OpenSoundSettings(void) { Spawn(L"X:\\Windows\\System32\\rundll32.exe shell32.dll,Control_RunDLL mmsys.cpl,,0"); }

/* ---------------------------------------------------------------- volume window */

static int VS(double v) { return (int)(v * V.s + 0.5); }

static HWND s_mixerBtn;

static void LayoutFlyout(int w, int h)
{
    if (V.font) DeleteObject(V.font);
    V.font = CreateFontW(-VS(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(V.label, WM_SETFONT, (WPARAM)V.font, FALSE);
    SendMessageW(V.muteBtn, WM_SETFONT, (WPARAM)V.font, FALSE);
    SendMessageW(s_mixerBtn, WM_SETFONT, (WPARAM)V.font, FALSE);
    MoveWindow(V.label, VS(12), VS(10), w - VS(24), VS(20), FALSE);
    MoveWindow(V.slider, VS(6), VS(32), w - VS(12), VS(30), FALSE);
    MoveWindow(V.muteBtn, VS(12), h - VS(34), VS(90), VS(26), FALSE);
    MoveWindow(s_mixerBtn, w - VS(12) - VS(90), h - VS(34), VS(90), VS(26), FALSE);
}

static void ShowFlyout(void)
{
    POINT pt;
    MONITORINFO mi;
    int w, h, x, y;
    if (!V.fly) return;
    if (IsWindowVisible(V.fly)) { ShowWindow(V.fly, SW_HIDE); return; }
    if (GetTickCount() - V.flyClosedAt < 300) return;   /* this click on the icon is what closed it */
    ReadState();
    GetCursorPos(&pt);
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    V.s = WindowDpi(V.fly) / 96.0;
    w = VS(300); h = VS(104);
    x = pt.x - w / 2; y = mi.rcWork.bottom - h - VS(8);   /* above the taskbar, centered on the click */
    if (x + w > mi.rcWork.right - VS(8)) x = mi.rcWork.right - VS(8) - w;
    if (x < mi.rcWork.left + VS(8)) x = mi.rcWork.left + VS(8);
    LayoutFlyout(w, h);
    SendMessageW(V.slider, TBM_SETPOS, TRUE, V.level < 0 ? 0 : V.level);
    EnableWindow(V.slider, V.level >= 0);
    EnableWindow(V.muteBtn, V.level >= 0);
    TrayUpdate(TRUE);
    SetWindowPos(V.fly, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(V.fly);
    SetFocus(V.slider);
    Log(L"tray-vol: volume window (%d%%%s)", V.level, V.muted ? L" muted" : L"");
}

static LRESULT CALLBACK FlyProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(h)) { ShowWindow(h, SW_HIDE); V.flyClosedAt = GetTickCount(); }
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == V.slider) {
            int pos = (int)SendMessageW(V.slider, TBM_GETPOS, 0, 0);
            SetLevel(pos);
            TrayUpdate(FALSE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_V_MUTE) { ToggleMute(); TrayUpdate(TRUE); }
        else if (LOWORD(wp) == IDC_V_MIXER) { ShowWindow(h, SW_HIDE); OpenMixer(); }
        else if (LOWORD(wp) == IDCANCEL) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Esc in the slider closes the window (the slider has the focus) */
static WNDPROC s_sliderProc;
static LRESULT CALLBACK SliderProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { ShowWindow(V.fly, SW_HIDE); return 0; }
    return CallWindowProcW(s_sliderProc, h, m, wp, lp);
}

static void TrayMenu(void)
{
    HMENU mn = CreatePopupMenu();
    POINT pt;
    wchar_t head[200];
    int cmd;
    if (!mn) return;
    ReadState();
    if (V.level < 0) wcscpy_s(head, _countof(head), L"No audio output device");
    else _snwprintf_s(head, _countof(head), _TRUNCATE, L"%s: %d%%%s", V.devName[0] ? V.devName : L"Speakers", V.level, V.muted ? L" (muted)" : L"");
    AppendMenuW(mn, MF_STRING | MF_GRAYED, 1, head);
    AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mn, MF_STRING | (V.muted ? MF_CHECKED : 0) | (V.level < 0 ? MF_GRAYED : 0), 20, L"Mute");
    AppendMenuW(mn, MF_STRING, 24, L"Volume mixer");
    AppendMenuW(mn, MF_STRING, 25, L"Sound settings");
    GetCursorPos(&pt);
    SetForegroundWindow(V.hwnd);
    cmd = (int)TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, V.hwnd, NULL);
    PostMessageW(V.hwnd, WM_NULL, 0, 0);
    DestroyMenu(mn);
    if (cmd == 20) { ToggleMute(); TrayUpdate(TRUE); }
    else if (cmd == 24) OpenMixer();
    else if (cmd == 25) OpenSoundSettings();
}

static LRESULT CALLBACK TrayVolProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == V.taskbarCreated && m) { Log(L"tray-vol: TaskbarCreated - re-adding the icon"); V.added = FALSE; TrayUpdate(TRUE); return 0; }
    switch (m) {
    case WM_TIMER:
        TrayUpdate(FALSE);
        return 0;
    case WM_TVCB:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowFlyout();
        else if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_COMMAND:   /* tests: 20 mute toggle, 21 volume window, 22 menu, 23 log the state */
        if (LOWORD(wp) == 20) { ToggleMute(); TrayUpdate(TRUE); }
        else if (LOWORD(wp) == 21) ShowFlyout();
        else if (LOWORD(wp) == 22) TrayMenu();
        else if (LOWORD(wp) == 23) { ReadState(); Log(L"tray-vol: state device='%s' level=%d muted=%d", V.devName, V.level, V.muted); }
        return 0;
    case WM_CLOSE:
        return 0;   /* no exit, like the network icon */
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int VolTray(void)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, TV_MUTEX);
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { Log(L"tray-vol: already running"); CloseHandle(mutex); return 0; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof(icc); icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    ZeroMemory(&V, sizeof(V));
    V.level = -2; V.iconLevel = -9; V.iconMuted = -1; V.s = 1.0;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = TrayVolProc; wc.hInstance = hi; wc.lpszClassName = TV_CLASS;
    if (!RegisterClassExW(&wc)) { Log(L"tray-vol: RegisterClass failed err=%lu", GetLastError()); return 1; }
    wc.lpfnWndProc = FlyProc; wc.lpszClassName = TV_FLYOUT; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&wc)) { Log(L"tray-vol: RegisterClass (window) failed err=%lu", GetLastError()); return 1; }
    V.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    /* hidden top-level window (TaskbarCreated is broadcast to top-level windows only) */
    V.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TV_CLASS, L"MyPE volume", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    V.fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, TV_FLYOUT, L"Volume", WS_POPUP | WS_BORDER, 0, 0, 300, 104, NULL, NULL, hi, NULL);
    if (!V.hwnd || !V.fly) { Log(L"tray-vol: CreateWindow failed err=%lu", GetLastError()); return 1; }
    V.label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, V.fly, (HMENU)(INT_PTR)IDC_V_LABEL, hi, NULL);
    V.slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0, 0, 10, 10, V.fly, (HMENU)(INT_PTR)IDC_V_SLIDER, hi, NULL);
    V.muteBtn = CreateWindowExW(0, L"BUTTON", L"Mute", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, V.fly, (HMENU)(INT_PTR)IDC_V_MUTE, hi, NULL);
    s_mixerBtn = CreateWindowExW(0, L"BUTTON", L"Mixer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, V.fly, (HMENU)(INT_PTR)IDC_V_MIXER, hi, NULL);
    SendMessageW(V.slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(V.slider, TBM_SETPAGESIZE, 0, 10);
    s_sliderProc = (WNDPROC)SetWindowLongPtrW(V.slider, GWLP_WNDPROC, (LONG_PTR)SliderProc);
    Log(L"tray-vol: started (hwnd %p)", (void *)V.hwnd);
    TrayUpdate(TRUE);
    SetTimer(V.hwnd, 1, 500, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    ReleaseDevice();
    if (V.en) IMMDeviceEnumerator_Release(V.en);
    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
