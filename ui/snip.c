/*
 * snip.c - screen snip tool in the style of the classic Snipping Tool (README.md). Own code, Win32/GDI + GDI+ (flat
 * API, image encoders) + the common Save dialog, in C.
 *
 *   WinPE-ShellUI.exe snip         the tool window: [New] [mode: Rectangular / Full-screen] [Save As...] [Copy] [Close]
 *   WinPE-ShellUI.exe snip /now    starts a snip at once, then shows the window with the result (Print Screen)
 *
 * New: the screen is frozen (all monitors); Rectangular = drag an area (a click without dragging takes the monitor under
 * the mouse), Full-screen = the monitor under the mouse. Esc or a right click cancels. The result is shown in the window
 * and copied to the clipboard (as the original tool does); it is saved only with Save As... (PNG, JPEG or BMP; default
 * folder: <boot stick>\MyPE\Screenshots when a drive has the marker MyPE\mype-stick, else the Desktop).
 * The Windows 11 Snipping Tool is a packaged (AppX) app and does not run in WinPE; the classic SnippingTool.exe is no
 * longer part of Windows 11. Per-monitor DPI aware: the capture is in physical pixels.
 */
#include "shellui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define TOOL_CLASS  L"WinPE-Shell.SnipTool"
#define SNIP_CLASS  L"WinPE-Shell.Snip"
#define SNIP_MUTEX  L"Local\\WinPE-Shell.Snip"
#define IDC_NEW   401
#define IDC_MODE  402
#define IDC_SAVE  403
#define IDC_COPY  404
#define IDC_CLOSE 405
#define WM_APP_SNIP (WM_APP + 1)

/* GDI+ flat API (gdiplus.dll, part of Windows): only what saving needs */
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupInput;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateBitmapFromHBITMAP(HBITMAP hbm, HPALETTE hpal, void **bitmap);
int WINAPI GdipSaveImageToFile(void *image, const WCHAR *filename, const CLSID *encoder, const void *params);
int WINAPI GdipDisposeImage(void *image);
static const CLSID s_enc[3] = {
    { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } },   /* PNG */
    { 0x557CF401, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } },   /* JPEG */
    { 0x557CF400, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } } }; /* BMP */

/* the overlay used while selecting */
static struct {
    HWND wnd;
    HBITMAP shot, dim;        /* frozen screen, and the same darkened */
    int vx, vy, vw, vh;       /* virtual screen */
    POINT a, b;               /* drag start / current, client coordinates */
    BOOL dragging, done, full;
    RECT result;              /* selected area, client coordinates */
} S;

/* the tool window */
static struct {
    HWND wnd, bNew, cMode, bSave, bCopy, bClose;
    HFONT font;
    HBITMAP img;              /* the last snip */
    int iw, ih;
    double s;
} T;

static int TS(double v) { return (int)(v * T.s + 0.5); }

static HBITMAP CopyArea(HBITMAP src, int x, int y, int w, int h)
{
    HDC screen = GetDC(NULL), s = CreateCompatibleDC(screen), d = CreateCompatibleDC(screen);
    HBITMAP out = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ os = SelectObject(s, src), od = SelectObject(d, out);
    BitBlt(d, 0, 0, w, h, s, x, y, SRCCOPY);
    SelectObject(s, os); SelectObject(d, od);
    DeleteDC(s); DeleteDC(d); ReleaseDC(NULL, screen);
    return out;
}

/* ------------------------------------------------------------------ selection overlay */

static BOOL Capture(void)
{
    HDC screen = GetDC(NULL), m = CreateCompatibleDC(screen), m2, b;
    HGDIOBJ o, ob;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 110, 0 };
    HBITMAP one;
    RECT r = { 0, 0, 0, 0 };
    S.vx = GetSystemMetrics(SM_XVIRTUALSCREEN); S.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    S.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN); S.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    S.shot = CreateCompatibleBitmap(screen, S.vw, S.vh);
    S.dim = CreateCompatibleBitmap(screen, S.vw, S.vh);
    if (!S.shot || !S.dim) { DeleteDC(m); ReleaseDC(NULL, screen); return FALSE; }
    o = SelectObject(m, S.shot);
    BitBlt(m, 0, 0, S.vw, S.vh, screen, S.vx, S.vy, SRCCOPY | CAPTUREBLT);
    m2 = CreateCompatibleDC(screen);
    SelectObject(m2, S.dim);
    BitBlt(m2, 0, 0, S.vw, S.vh, m, 0, 0, SRCCOPY);
    b = CreateCompatibleDC(screen);                       /* darken: a black layer at ~43 % */
    one = CreateCompatibleBitmap(screen, 1, 1);
    ob = SelectObject(b, one);
    SetRect(&r, 0, 0, 1, 1); FillRect(b, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    AlphaBlend(m2, 0, 0, S.vw, S.vh, b, 0, 0, 1, 1, bf);
    SelectObject(b, ob); DeleteObject(one); DeleteDC(b);
    DeleteDC(m2);
    SelectObject(m, o); DeleteDC(m); ReleaseDC(NULL, screen);
    return TRUE;
}

static void SelRect(RECT *r)
{
    r->left = min(S.a.x, S.b.x); r->right = max(S.a.x, S.b.x);
    r->top = min(S.a.y, S.b.y); r->bottom = max(S.a.y, S.b.y);
}

static void MonitorUnderPoint(POINT client, RECT *out)
{
    POINT p = { client.x + S.vx, client.y + S.vy };
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    SetRect(out, mi.rcMonitor.left - S.vx, mi.rcMonitor.top - S.vy, mi.rcMonitor.right - S.vx, mi.rcMonitor.bottom - S.vy);
}

static void PaintOverlay(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem = CreateCompatibleDC(dc), back = CreateCompatibleDC(dc);
    HBITMAP buf = CreateCompatibleBitmap(dc, S.vw, S.vh);
    HGDIOBJ ob = SelectObject(back, buf), om = SelectObject(mem, S.dim);
    RECT r = { 0, 0, 0, 0 };
    BOOL show = FALSE;
    BitBlt(back, 0, 0, S.vw, S.vh, mem, 0, 0, SRCCOPY);
    if (S.full) {                                        /* full-screen mode: the monitor under the mouse lights up */
        POINT c; GetCursorPos(&c); c.x -= S.vx; c.y -= S.vy;
        MonitorUnderPoint(c, &r); show = TRUE;
    } else if (S.dragging) { SelRect(&r); show = TRUE; }
    if (show) {
        HPEN pen = CreatePen(PS_SOLID, max(1, (int)(WindowDpi(h) / 96)), RGB(0, 120, 215));
        HGDIOBJ op, obr;
        SelectObject(mem, S.shot);
        BitBlt(back, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, r.left, r.top, SRCCOPY);
        op = SelectObject(back, pen); obr = SelectObject(back, GetStockObject(NULL_BRUSH));
        Rectangle(back, r.left, r.top, r.right, r.bottom);
        SelectObject(back, op); SelectObject(back, obr); DeleteObject(pen);
    }
    BitBlt(dc, 0, 0, S.vw, S.vh, back, 0, 0, SRCCOPY);
    SelectObject(mem, om); SelectObject(back, ob);
    DeleteObject(buf); DeleteDC(mem); DeleteDC(back);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
        return TRUE;
    case WM_LBUTTONDOWN:
        S.a.x = S.b.x = (short)LOWORD(lp); S.a.y = S.b.y = (short)HIWORD(lp);
        if (!S.full) { S.dragging = TRUE; SetCapture(h); }
        return 0;
    case WM_MOUSEMOVE:
        if (S.dragging) { S.b.x = (short)LOWORD(lp); S.b.y = (short)HIWORD(lp); InvalidateRect(h, NULL, FALSE); }
        else if (S.full) InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (S.dragging) { ReleaseCapture(); S.dragging = FALSE; }
        S.b.x = (short)LOWORD(lp); S.b.y = (short)HIWORD(lp);
        SelRect(&S.result);
        if (S.full || S.result.right - S.result.left < 4 || S.result.bottom - S.result.top < 4)
            MonitorUnderPoint(S.b, &S.result);          /* full-screen mode, or a click: the whole monitor */
        S.done = TRUE;
        DestroyWindow(h);
        return 0;
    case WM_RBUTTONUP:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintOverlay(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* runs one selection (own message loop until the overlay closes); returns the snip or NULL (cancelled / failed) */
static HBITMAP RunSelection(BOOL full)
{
    MSG msg;
    HBITMAP area = NULL;
    ZeroMemory(&S, sizeof(S));
    S.full = full;
    if (!Capture()) { Log(L"snip: screen capture failed"); return NULL; }
    S.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, SNIP_CLASS, L"Snip", WS_POPUP,
                            S.vx, S.vy, S.vw, S.vh, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!S.wnd) { Log(L"snip: overlay failed err=%lu", GetLastError()); goto out; }
    ShowWindow(S.wnd, SW_SHOW);
    ForceForeground(S.wnd);
    while (IsWindow(S.wnd) && GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (S.done) {
        int w = S.result.right - S.result.left, h = S.result.bottom - S.result.top;
        area = CopyArea(S.shot, S.result.left, S.result.top, w, h);
        Log(L"snip: %dx%d at %d,%d (%s)", w, h, S.result.left + S.vx, S.result.top + S.vy, full ? L"full screen" : L"rectangle");
    } else Log(L"snip: cancelled");
out:
    if (S.shot) DeleteObject(S.shot);
    if (S.dim) DeleteObject(S.dim);
    S.shot = S.dim = NULL;
    return area;
}

/* ------------------------------------------------------------------ clipboard, saving */

static BOOL ToClipboard(HBITMAP bmp)
{
    BOOL ok = FALSE;
    BITMAP bm; HBITMAP copy;
    if (!bmp || !OpenClipboard(T.wnd)) return FALSE;
    EmptyClipboard();
    GetObjectW(bmp, sizeof(bm), &bm);
    copy = CopyArea(bmp, 0, 0, bm.bmWidth, bm.bmHeight);   /* the clipboard owns this copy */
    if (copy && SetClipboardData(CF_BITMAP, copy)) ok = TRUE; else if (copy) DeleteObject(copy);
    CloseClipboard();
    return ok;
}

/* default folder of Save As: <boot stick>\MyPE\Screenshots (drive with the marker MyPE\mype-stick), else the Desktop */
static void DefaultFolder(wchar_t *dir, size_t cch)
{
    wchar_t root[4] = L"C:\\", p[MAX_PATH];
    DWORD drives = GetLogicalDrives();
    UINT old = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    int d;
    dir[0] = 0;
    for (d = 2; d < 26 && !dir[0]; d++) {
        UINT t;
        if (!(drives & (1u << d)) || d == 'X' - 'A') continue;
        root[0] = (wchar_t)(L'A' + d);
        t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%sMyPE\\mype-stick", root);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
        _snwprintf_s(dir, cch, _TRUNCATE, L"%sMyPE\\Screenshots", root);
        CreateDirectoryW(dir, NULL);
    }
    SetErrorMode(old);
    if (!dir[0] && !SHGetSpecialFolderPathW(NULL, dir, CSIDL_DESKTOPDIRECTORY, TRUE)) wcscpy_s(dir, cch, L"X:\\");
}

static void SaveAs(void)
{
    wchar_t file[MAX_PATH], dir[MAX_PATH], msg[MAX_PATH + 64];
    OPENFILENAMEW ofn;
    SYSTEMTIME t;
    GpStartupInput in = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token = 0;
    void *gimg = NULL;
    int st, kind;
    const wchar_t *ext;
    if (!T.img) return;
    DefaultFolder(dir, _countof(dir));
    GetLocalTime(&t);
    _snwprintf_s(file, _countof(file), _TRUNCATE, L"Screenshot %04u-%02u-%02u %02u%02u%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = T.wnd;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0JPEG image (*.jpg)\0*.jpg\0Bitmap (*.bmp)\0*.bmp\0";
    ofn.nFilterIndex = 1; ofn.lpstrFile = file; ofn.nMaxFile = _countof(file); ofn.lpstrInitialDir = dir;
    ofn.lpstrDefExt = L"png"; ofn.lpstrTitle = L"Save snip as";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) { DWORD e = CommDlgExtendedError(); if (e) Log(L"snip: Save dialog failed (0x%lx)", e); return; }
    /* the encoder follows the extension typed, else the chosen filter */
    ext = wcsrchr(file, L'.');
    kind = (int)ofn.nFilterIndex - 1;
    if (ext && (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg"))) kind = 1;
    else if (ext && !_wcsicmp(ext, L".bmp")) kind = 2;
    else if (ext && !_wcsicmp(ext, L".png")) kind = 0;
    if (kind < 0 || kind > 2) kind = 0;
    if (GdiplusStartup(&token, &in, NULL) != 0) { Message(L"Saving needs GDI+ (gdiplus.dll), which is missing."); return; }
    st = GdipCreateBitmapFromHBITMAP(T.img, NULL, &gimg);
    if (st == 0) st = GdipSaveImageToFile(gimg, file, &s_enc[kind], NULL);
    if (gimg) GdipDisposeImage(gimg);
    GdiplusShutdown(token);
    Log(L"snip: save %s -> GDI+ status %d", file, st);
    if (st != 0) { _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Could not save\n%s\n(GDI+ status %d).", file, st); Message(L"%s", msg); }
    else { _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"Snipping Tool - saved %s", wcsrchr(file, L'\\') ? wcsrchr(file, L'\\') + 1 : file); SetWindowTextW(T.wnd, msg); }
}

/* ------------------------------------------------------------------ tool window */

static void ToolLayout(void)
{
    RECT wr, cr;
    MONITORINFO mi;
    int pad = TS(8), bh = TS(30), x = pad, top = TS(44), w, h, maxW, maxH, vw, vh;
    HFONT old = T.font;
    T.font = CreateFontW(-TS(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(T.bNew, WM_SETFONT, (WPARAM)T.font, FALSE); SendMessageW(T.cMode, WM_SETFONT, (WPARAM)T.font, FALSE);
    SendMessageW(T.bSave, WM_SETFONT, (WPARAM)T.font, FALSE); SendMessageW(T.bCopy, WM_SETFONT, (WPARAM)T.font, FALSE);
    SendMessageW(T.bClose, WM_SETFONT, (WPARAM)T.font, FALSE);
    if (old) DeleteObject(old);
    MoveWindow(T.bNew, x, pad, TS(70), bh, FALSE); x += TS(70) + pad;
    MoveWindow(T.cMode, x, pad + TS(2), TS(170), TS(200), FALSE); x += TS(170) + pad;
    MoveWindow(T.bSave, x, pad, TS(90), bh, FALSE); x += TS(90) + pad;
    MoveWindow(T.bCopy, x, pad, TS(70), bh, FALSE); x += TS(70) + pad;
    MoveWindow(T.bClose, x, pad, TS(70), bh, FALSE); x += TS(70) + pad;
    /* window: the toolbar, plus the snip scaled to fit 80 % of the monitor */
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(T.wnd, MONITOR_DEFAULTTONEAREST), &mi);
    maxW = (mi.rcWork.right - mi.rcWork.left) * 8 / 10; maxH = (mi.rcWork.bottom - mi.rcWork.top) * 8 / 10 - top;
    vw = T.iw; vh = T.ih;
    if (T.img && (vw > maxW || vh > maxH)) {
        double k = min((double)maxW / vw, (double)maxH / vh);
        vw = (int)(vw * k); vh = (int)(vh * k);
    }
    w = max(x, T.img ? vw + 2 * pad : x);
    h = top + (T.img ? vh + pad : 0);
    GetWindowRect(T.wnd, &wr); GetClientRect(T.wnd, &cr);
    SetWindowPos(T.wnd, NULL, 0, 0, (wr.right - wr.left) - cr.right + w, (wr.bottom - wr.top) - cr.bottom + h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(T.wnd, &wr);                            /* keep it on the screen */
    if (wr.right > mi.rcWork.right || wr.bottom > mi.rcWork.bottom)
        SetWindowPos(T.wnd, NULL, max(mi.rcWork.left, mi.rcWork.right - (wr.right - wr.left)),
                     max(mi.rcWork.top, mi.rcWork.bottom - (wr.bottom - wr.top)), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnableWindow(T.bSave, T.img != NULL); EnableWindow(T.bCopy, T.img != NULL);
    InvalidateRect(T.wnd, NULL, TRUE);
}

static void NewSnip(void)
{
    BOOL full = SendMessageW(T.cMode, CB_GETCURSEL, 0, 0) == 1;
    HBITMAP area;
    ShowWindow(T.wnd, SW_HIDE);
    Sleep(250);                                           /* let the screen repaint without the tool window */
    area = RunSelection(full);
    if (area) {
        BITMAP bm;
        if (T.img) DeleteObject(T.img);
        T.img = area;
        GetObjectW(area, sizeof(bm), &bm); T.iw = bm.bmWidth; T.ih = bm.bmHeight;
        Log(L"snip: clipboard %s", ToClipboard(T.img) ? L"ok" : L"FAILED");
        SetWindowTextW(T.wnd, L"Snipping Tool - copied to the clipboard (Save As... to keep it)");
    }
    ToolLayout();
    ShowWindow(T.wnd, SW_SHOW);
    ForceForeground(T.wnd);
    SetFocus(T.img ? T.bSave : T.bNew);
}

static void PaintTool(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    if (T.img) {
        RECT cr; HDC mem = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(mem, T.img);
        int top = TS(44), pad = TS(8), vw, vh;
        GetClientRect(h, &cr);
        vw = cr.right - 2 * pad; vh = cr.bottom - top - pad;
        if ((double)T.iw / T.ih > (double)vw / vh) vh = (int)((double)vw * T.ih / T.iw); else vw = (int)((double)vh * T.iw / T.ih);
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, (cr.right - vw) / 2, top, vw, vh, mem, 0, 0, T.iw, T.ih, SRCCOPY);
        SelectObject(mem, o); DeleteDC(mem);
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK ToolProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_NEW: NewSnip(); break;
        case IDC_SAVE: SaveAs(); break;
        case IDC_COPY: if (T.img) Log(L"snip: copy %s", ToClipboard(T.img) ? L"ok" : L"FAILED"); break;
        case IDC_CLOSE: DestroyWindow(h); break;
        }
        return 0;
    case WM_APP_SNIP:
        NewSnip();
        return 0;
    case WM_PAINT:
        PaintTool(h);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        T.s = HIWORD(wp) / 96.0;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ToolLayout();
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int SnipCommand(int argc, wchar_t **argv)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, SNIP_MUTEX);
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MONITORINFO mi;
    POINT pt;
    MSG msg;
    BOOL now = argc > 2 && (!_wcsicmp(argv[2], L"/now") || !_wcsicmp(argv[2], L"-now"));
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {   /* one tool: Print Screen in a running tool = a new snip */
        HWND prev = FindWindowW(TOOL_CLASS, NULL);
        if (prev) { if (now) PostMessageW(prev, WM_APP_SNIP, 0, 0); else ForceForeground(prev); }
        Log(L"snip: already running");
        CloseHandle(mutex);
        return 0;
    }
    icc.dwSize = sizeof(icc); icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    ZeroMemory(&T, sizeof(T)); T.s = 1.0;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = OverlayProc; wc.hInstance = hi; wc.lpszClassName = SNIP_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = ToolProc; wc.lpszClassName = TOOL_CLASS; wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassExW(&wc);
    GetCursorPos(&pt); mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    T.wnd = CreateWindowExW(WS_EX_APPWINDOW, TOOL_CLASS, L"Snipping Tool", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left) / 4, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 6,
                            400, 100, NULL, NULL, hi, NULL);
    if (!T.wnd) { Log(L"snip: CreateWindow failed err=%lu", GetLastError()); goto done; }
    T.s = WindowDpi(T.wnd) / 96.0;
    T.bNew = CreateWindowExW(0, L"BUTTON", L"New", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, T.wnd, (HMENU)(INT_PTR)IDC_NEW, hi, NULL);
    T.cMode = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 10, 10, T.wnd, (HMENU)(INT_PTR)IDC_MODE, hi, NULL);
    T.bSave = CreateWindowExW(0, L"BUTTON", L"Save As...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, T.wnd, (HMENU)(INT_PTR)IDC_SAVE, hi, NULL);
    T.bCopy = CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, T.wnd, (HMENU)(INT_PTR)IDC_COPY, hi, NULL);
    T.bClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, T.wnd, (HMENU)(INT_PTR)IDC_CLOSE, hi, NULL);
    SendMessageW(T.cMode, CB_ADDSTRING, 0, (LPARAM)L"Rectangular snip");
    SendMessageW(T.cMode, CB_ADDSTRING, 0, (LPARAM)L"Full-screen snip");
    SendMessageW(T.cMode, CB_SETCURSEL, 0, 0);
    ToolLayout();
    Log(L"snip: tool open%s (dpi-scale=%.2f)", now ? L", snip now" : L"", T.s);
    if (now) PostMessageW(T.wnd, WM_APP_SNIP, 0, 0);
    else { ShowWindow(T.wnd, SW_SHOW); ForceForeground(T.wnd); SetFocus(T.bNew); }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && GetAncestor(msg.hwnd, GA_ROOT) == T.wnd) { DestroyWindow(T.wnd); continue; }
        if (!IsDialogMessageW(T.wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
done:
    if (T.img) DeleteObject(T.img);
    if (T.font) DeleteObject(T.font);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
