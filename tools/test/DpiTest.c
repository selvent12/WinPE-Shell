/*
 * DpiTest.c - VM test tool: can the display scale be changed LIVE in WinPE (no new session)?
 * Uses the per-monitor scale of the display configuration API, as the Settings app does
 * (DisplayConfigGetDeviceInfo / DisplayConfigSetDeviceInfo with the undocumented types -3 = get DPI scale,
 * -4 = set DPI scale; scale given relative to the recommended one, in steps of the list below).
 *   DpiTest.exe            print the scale of every active display
 *   DpiTest.exe <percent>  set every active display to <percent>, then print again (after 2 s)
 * License: LGPL-2.1+ (WinPE-Shell test tools).
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "advapi32.lib")

static const int s_vals[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
#define NVALS ((int)(sizeof(s_vals) / sizeof(s_vals[0])))

typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 minRel, curRel, maxRel; } DpiGet;
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 rel; } DpiSet;

static void Report(const wchar_t *when)
{
    POINT pt = { 0, 0 };
    UINT dx = 0, dy = 0;
    HRESULT hr = GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dx, &dy);
    HKEY k; DWORD lp = 0, cb = sizeof(lp);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &k)) { RegQueryValueExW(k, L"LogPixels", NULL, NULL, (BYTE *)&lp, &cb); RegCloseKey(k); }
    wprintf(L"[%s] primary monitor effective DPI=%u (hr=0x%08lX) system DPI=%u HKCU LogPixels=%lu\n", when, dx, (unsigned long)hr, GetDpiForSystem(), lp);
}

int wmain(int argc, wchar_t **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int want = argc > 1 ? _wtoi(argv[1]) : 0;
    UINT32 np = 0, nm = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm);
    if (rc) { wprintf(L"GetDisplayConfigBufferSizes rc=%ld\n", rc); return 1; }
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof(*paths));
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof(*modes));
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
    if (rc) { wprintf(L"QueryDisplayConfig rc=%ld\n", rc); return 1; }
    Report(L"before");
    int failed = 0;
    for (UINT32 i = 0; i < np; i++) {
        DpiGet g = { 0 };
        g.h.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3; g.h.size = sizeof(g);
        g.h.adapterId = paths[i].sourceInfo.adapterId; g.h.id = paths[i].sourceInfo.id;
        rc = DisplayConfigGetDeviceInfo(&g.h);
        int rec = -g.minRel;    /* index of the recommended scale */
        int cur = rec + g.curRel;
        wprintf(L"display %u: get rc=%ld min=%d cur=%d max=%d -> recommended %d%%, current %d%%\n", i, rc, g.minRel, g.curRel, g.maxRel,
                rec >= 0 && rec < NVALS ? s_vals[rec] : -1, cur >= 0 && cur < NVALS ? s_vals[cur] : -1);
        if (!want || rc) continue;
        int idx = -1;
        for (int k = 0; k < NVALS; k++) if (s_vals[k] == want) idx = k;
        if (idx < 0) { wprintf(L"  %d%% is not a valid scale\n", want); failed = 1; continue; }
        int rel = idx - rec;
        if (rel < g.minRel || rel > g.maxRel) wprintf(L"  note: %d%% is outside the range Windows offers for this display\n", want);
        DpiSet s = { 0 };
        s.h.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-4; s.h.size = sizeof(s);
        s.h.adapterId = g.h.adapterId; s.h.id = g.h.id; s.rel = rel;
        rc = DisplayConfigSetDeviceInfo(&s.h);
        wprintf(L"  set %d%% (rel %d): rc=%ld\n", want, rel, rc);
        if (rc) failed = 1;
    }
    if (want) {
        Sleep(2000);
        Report(L"after 2 s");
        for (UINT32 i = 0; i < np; i++) {
            DpiGet g = { 0 };
            g.h.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3; g.h.size = sizeof(g);
            g.h.adapterId = paths[i].sourceInfo.adapterId; g.h.id = paths[i].sourceInfo.id;
            rc = DisplayConfigGetDeviceInfo(&g.h);
            int cur = -g.minRel + g.curRel;
            wprintf(L"display %u after: rc=%ld current %d%%\n", i, rc, cur >= 0 && cur < NVALS ? s_vals[cur] : -1);
        }
    }
    return failed;
}
