/*
 * bat.c - battery tray icon for laptops (README.md). Own code, Win32/GDI. WinPE has no battery icon of its own.
 *
 *   WinPE-ShellUI.exe tray-bat      tray icon (started at boot, single instance); ends at once when the system has no
 *                                   battery (desktop PC), so it can be started everywhere
 *
 * - icon: battery outline filled by the charge (green, orange at 20 % or less, red at 10 % or less), a plug mark
 *   while on AC power; tooltip: "Battery 76 % - charging" / "... - 1 h 20 min left" / "... - plugged in";
 * - left or right click: a small menu with the same information;
 * - GetSystemPowerStatus on WM_POWERBROADCAST and every 30 s. Log: "tray-bat: ...".
 */
#include "shellui.h"
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

#define TB_CLASS   L"WinPE-Shell.TrayBat"
#define TB_MUTEX   L"Local\\WinPE-Shell.TrayBat"
#define WM_TBCB    (WM_APP + 1)

static struct {
    HWND hwnd;
    HICON icon;
    int iconPct, iconAc, iconSize;
    BOOL added;
    UINT taskbarCreated;
    SYSTEM_POWER_STATUS ps;
} T;

static int SmallIcon(void)
{
    typedef int (WINAPI *Fn)(int, UINT);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    Fn f = u ? (Fn)(void *)GetProcAddress(u, "GetSystemMetricsForDpi") : NULL;
    UINT dpi = T.hwnd ? WindowDpi(T.hwnd) : 96;
    return f && dpi ? f(SM_CXSMICON, dpi) : GetSystemMetrics(SM_CXSMICON);
}

/* 32-bit icon: outline (white), fill by pct, plug mark (small bar left of the body) on AC */
static HICON MakeBatIcon(int pct, BOOL ac, int sz)
{
    BITMAPV5HEADER bh;
    DWORD *bits = NULL, fill;
    HDC dc;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic;
    int x, y;
    /* body (outline) and terminal in pixels */
    int l = sz * 2 / 16, r = sz * 13 / 16, t = sz * 4 / 16, b = sz * 12 / 16, th = sz >= 24 ? 2 : 1;
    int inner = r - l - 2 * th - 2, fw = pct < 0 ? 0 : (inner * pct + 50) / 100;
    fill = pct >= 0 && pct <= 10 ? 0xE04848 : (pct >= 0 && pct <= 20 ? 0xF0A030 : 0x4CC24C);
    ZeroMemory(&bh, sizeof(bh));
    bh.bV5Size = sizeof(bh); bh.bV5Width = sz; bh.bV5Height = -sz; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS; bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    ZeroMemory(bits, (size_t)sz * sz * 4);
    for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
        DWORD c = 0;
        BOOL inBody = x >= l && x < r && y >= t && y < b;
        BOOL edge = inBody && (x < l + th || x >= r - th || y < t + th || y >= b - th);
        BOOL term = x >= r && x < r + th + 1 && y >= t + (b - t) / 3 && y < b - (b - t) / 3;
        BOOL fl = x >= l + th + 1 && x < l + th + 1 + fw && y >= t + th + 1 && y < b - th - 1;
        BOOL plug = ac && x >= 0 && x < th + 1 && y >= t + (b - t) / 3 && y < b - (b - t) / 3;
        if (edge || term) c = 0xFFFFFF;
        else if (fl) c = fill;
        else if (plug) c = 0xFFFFFF;
        if (c) bits[y * sz + x] = 0xFF000000 | c;
    }
    mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return ic;
}

/* "Battery 76% - 1 h 20 min left" etc. */
static void StatusText(wchar_t *s, size_t n)
{
    int pct = T.ps.BatteryLifePercent <= 100 ? T.ps.BatteryLifePercent : -1;
    wchar_t p[16] = L"?";
    if (pct >= 0) _snwprintf_s(p, _countof(p), _TRUNCATE, L"%d%%", pct);
    if (T.ps.BatteryFlag & 8) _snwprintf_s(s, n, _TRUNCATE, L"Battery %s - charging", p);
    else if (T.ps.ACLineStatus == 1) _snwprintf_s(s, n, _TRUNCATE, L"Battery %s - plugged in", p);
    else if (T.ps.BatteryLifeTime != (DWORD)-1)
        _snwprintf_s(s, n, _TRUNCATE, L"Battery %s - %lu h %02lu min left", p, T.ps.BatteryLifeTime / 3600, (T.ps.BatteryLifeTime % 3600) / 60);
    else _snwprintf_s(s, n, _TRUNCATE, L"Battery %s - on battery", p);
}

static void Update(BOOL force)
{
    NOTIFYICONDATAW nd;
    int pct, ac, sz;
    if (!GetSystemPowerStatus(&T.ps)) return;
    pct = T.ps.BatteryLifePercent <= 100 ? T.ps.BatteryLifePercent : -1;
    ac = T.ps.ACLineStatus == 1;
    sz = SmallIcon();
    if (force || !T.icon || pct != T.iconPct || ac != T.iconAc || sz != T.iconSize) {
        HICON old = T.icon;
        T.icon = MakeBatIcon(pct, ac, sz);
        T.iconPct = pct; T.iconAc = ac; T.iconSize = sz;
        if (old) DestroyIcon(old);
    }    /* the tooltip (time left) changes without a new icon: always modified */
    ZeroMemory(&nd, sizeof(nd));
    nd.cbSize = sizeof(nd); nd.hWnd = T.hwnd; nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nd.uCallbackMessage = WM_TBCB; nd.hIcon = T.icon;
    StatusText(nd.szTip, _countof(nd.szTip));
    if (T.added && Shell_NotifyIconW(NIM_MODIFY, &nd)) return;
    T.added = Shell_NotifyIconW(NIM_ADD, &nd);
    if (T.added) Log(L"tray-bat: icon added (%s)", nd.szTip);
}

static void InfoMenu(void)
{
    HMENU m = CreatePopupMenu();
    wchar_t s[128];
    POINT pt;
    StatusText(s, _countof(s));
    AppendMenuW(m, MF_STRING | MF_GRAYED, 1, s);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 2, T.ps.ACLineStatus == 1 ? L"Power: AC adapter" : (T.ps.ACLineStatus == 0 ? L"Power: battery" : L"Power: unknown"));
    if (T.ps.SystemStatusFlag & 1) AppendMenuW(m, MF_STRING | MF_GRAYED, 3, L"Battery saver is on");
    GetCursorPos(&pt);
    SetForegroundWindow(T.hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, T.hwnd, NULL);
    PostMessageW(T.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK TrayBatProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == T.taskbarCreated && m) { Log(L"tray-bat: TaskbarCreated - re-adding the icon"); T.added = FALSE; Update(TRUE); return 0; }
    switch (m) {
    case WM_TIMER:
        Update(FALSE);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMPOWERSTATUSCHANGE) Update(FALSE);
        return TRUE;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        Update(TRUE);
        return 0;
    case WM_TBCB:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) InfoMenu();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int BatTray(void)
{
    HANDLE mutex;
    WNDCLASSW wc;
    MSG msg;
    NOTIFYICONDATAW nd;
    SYSTEM_POWER_STATUS ps;
    HINSTANCE hi = GetModuleHandleW(NULL);

    if (!GetSystemPowerStatus(&ps) || (ps.BatteryFlag & 128) || ps.BatteryFlag == 255) {
        Log(L"tray-bat: no system battery (flag %u) - no icon", (unsigned)ps.BatteryFlag);
        return 0;
    }
    mutex = CreateMutexW(NULL, TRUE, TB_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { Log(L"tray-bat: already running"); CloseHandle(mutex); return 0; }
    ZeroMemory(&T, sizeof(T));
    T.iconPct = -9;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TrayBatProc; wc.hInstance = hi; wc.lpszClassName = TB_CLASS;
    RegisterClassW(&wc);
    T.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    /* hidden top-level window: TaskbarCreated and WM_POWERBROADCAST go to top-level windows */
    T.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TB_CLASS, L"WinPE-Shell battery", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hi, NULL);
    if (!T.hwnd) { Log(L"tray-bat: CreateWindow failed err=%lu", GetLastError()); if (mutex) CloseHandle(mutex); return 1; }
    Log(L"tray-bat: started");
    Update(TRUE);
    SetTimer(T.hwnd, 1, 30000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    ZeroMemory(&nd, sizeof(nd));
    nd.cbSize = sizeof(nd); nd.hWnd = T.hwnd; nd.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nd);
    if (T.icon) DestroyIcon(T.icon);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
