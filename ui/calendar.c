/*
 * calendar.c - calendar flyout (README.md). Own code, Win32 + the common controls month calendar.
 *
 *   WinPE-ShellUI.exe calendar      the current month (today marked), the date written out and the time; opens above
 *                                   the clock of the monitor under the mouse, closes on Esc, a click elsewhere or a
 *                                   second "calendar" (the shell's clock click runs it: JS_TASKBAR.clock_command)
 * Per-monitor DPI aware like the rest of the utility: sizes come from the month calendar itself at the window's DPI.
 */
#include "shellui.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define CAL_CLASS  L"WinPE-Shell.Calendar"
#define CAL_MUTEX  L"Local\\WinPE-Shell.Calendar"
#define IDC_CAL_MONTH 301
#define IDC_CAL_TEXT  302
#define CAL_TIMER     1

static struct {
    HWND wnd, month, text;
    HFONT font, big;
    double s;
} C;

static int CS(double v) { return (int)(v * C.s + 0.5); }

static void CalText(void)
{
    wchar_t date[128] = L"", time[64] = L"", line[256];
    SYSTEMTIME t;
    GetLocalTime(&t);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &t, NULL, date, _countof(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &t, NULL, time, _countof(time));
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s\n%s", time, date);
    SetWindowTextW(C.text, line);
}

/* fonts at the window's DPI, then the size the month calendar asks for; window placed above the taskbar */
static void CalLayout(const RECT *work)
{
    RECT mc = { 0 };
    int pad, textH, w, h, x, y;
    HFONT oldF = C.font, oldB = C.big;
    C.s = WindowDpi(C.wnd) / 96.0;
    C.font = CreateFontW(-CS(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    C.big = CreateFontW(-CS(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(C.month, WM_SETFONT, (WPARAM)C.font, TRUE);
    SendMessageW(C.text, WM_SETFONT, (WPARAM)C.big, TRUE);
    if (oldF) DeleteObject(oldF);
    if (oldB) DeleteObject(oldB);
    SendMessageW(C.month, MCM_GETMINREQRECT, 0, (LPARAM)&mc);
    pad = CS(12); textH = CS(52);
    w = (mc.right - mc.left) + 2 * pad;
    h = textH + (mc.bottom - mc.top) + 3 * pad;
    if (w < CS(260)) w = CS(260);
    MoveWindow(C.text, pad, pad, w - 2 * pad, textH, FALSE);
    MoveWindow(C.month, (w - (mc.right - mc.left)) / 2, 2 * pad + textH, mc.right - mc.left, mc.bottom - mc.top, FALSE);
    x = work->right - w - CS(12); y = work->bottom - h - CS(12);
    if (x < work->left) x = work->left;
    if (y < work->top) y = work->top;
    SetWindowPos(C.wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static LRESULT CALLBACK CalProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(h);   /* a click elsewhere closes it */
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_TIMER:
        CalText();
        return 0;
    case WM_DPICHANGED: {
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        CalLayout(&mi.rcWork);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int CalendarWindow(void)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, CAL_MUTEX);
    HINSTANCE hi = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    MONITORINFO mi;
    POINT pt;
    MSG msg;
    HWND prev;
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {   /* second click on the clock: close the open one */
        prev = FindWindowW(CAL_CLASS, NULL);
        if (prev) PostMessageW(prev, WM_CLOSE, 0, 0);
        Log(L"calendar: already open - closed");
        CloseHandle(mutex);
        return 0;
    }
    icc.dwSize = sizeof(icc); icc.dwICC = ICC_DATE_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    ZeroMemory(&C, sizeof(C)); C.s = 1.0;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = CalProc; wc.hInstance = hi; wc.lpszClassName = CAL_CLASS;
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&wc)) { Log(L"calendar: RegisterClass failed err=%lu", GetLastError()); goto done; }
    GetCursorPos(&pt);
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    /* created on the target monitor (hidden), so its DPI is the one of that monitor */
    C.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CAL_CLASS, L"Calendar", WS_POPUP | WS_BORDER,
                            mi.rcWork.left, mi.rcWork.top, 10, 10, NULL, NULL, hi, NULL);
    if (!C.wnd) { Log(L"calendar: CreateWindow failed err=%lu", GetLastError()); goto done; }
    C.text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10,
                             C.wnd, (HMENU)(INT_PTR)IDC_CAL_TEXT, hi, NULL);
    C.month = CreateWindowExW(0, MONTHCAL_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                              C.wnd, (HMENU)(INT_PTR)IDC_CAL_MONTH, hi, NULL);
    if (!C.month) { Log(L"calendar: month calendar control failed err=%lu", GetLastError()); DestroyWindow(C.wnd); goto done; }
    CalText();
    CalLayout(&mi.rcWork);
    ShowWindow(C.wnd, SW_SHOW);
    ForceForeground(C.wnd);
    SetFocus(C.month);
    SetTimer(C.wnd, CAL_TIMER, 1000, NULL);
    Log(L"calendar: open (dpi-scale=%.2f)", C.s);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(C.wnd); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
done:
    if (C.font) DeleteObject(C.font);
    if (C.big) DeleteObject(C.big);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
