/*
 * scale.c - live display scale (see scale.h). Own code.
 * DisplayConfigGetDeviceInfo / DisplayConfigSetDeviceInfo with the undocumented device info types -3 (get DPI scale)
 * and -4 (set DPI scale), the interface the Settings app uses. The scale is relative to the display's recommended
 * one, in steps of g_scaleSteps; min/max say how far Windows lets it go (e.g. 1920x1080: 100..175 %).
 */
#include "mypeui.h"
#include "scale.h"
#include <stdlib.h>
#include <string.h>

const int g_scaleSteps[SCALE_MAXSTEPS] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 minRel, curRel, maxRel; } ScaleGet;
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER h; INT32 rel; } ScaleSetInfo;

typedef struct { LUID adapter; UINT32 id; BOOL primary; } ScaleSrc;

static int Paths(ScaleSrc *out, int max)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    int n = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm);
    if (rc != ERROR_SUCCESS) { Log(L"scale: GetDisplayConfigBufferSizes rc=%ld", rc); return 0; }
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np ? np : 1, sizeof(*paths));
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof(*modes));
    if (!paths || !modes) { free(paths); free(modes); return 0; }
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL);
    if (rc != ERROR_SUCCESS) { Log(L"scale: QueryDisplayConfig rc=%ld", rc); np = 0; }
    for (i = 0; i < np && n < max; i++) {
        UINT32 mi = paths[i].sourceInfo.modeInfoIdx;
        int k, dup = 0;
        for (k = 0; k < n; k++)     /* duplicate mode: two paths, one source */
            if (out[k].id == paths[i].sourceInfo.id && !memcmp(&out[k].adapter, &paths[i].sourceInfo.adapterId, sizeof(LUID))) dup = 1;
        if (dup) continue;
        out[n].adapter = paths[i].sourceInfo.adapterId;
        out[n].id = paths[i].sourceInfo.id;
        out[n].primary = mi < nm && modes[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                         modes[mi].sourceMode.position.x == 0 && modes[mi].sourceMode.position.y == 0;
        n++;
    }
    free(paths);
    free(modes);
    return n;
}

static BOOL Get(const ScaleSrc *s, ScaleGet *g)
{
    LONG rc;
    ZeroMemory(g, sizeof(*g));
    g->h.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3;
    g->h.size = sizeof(*g);
    g->h.adapterId = s->adapter;
    g->h.id = s->id;
    rc = DisplayConfigGetDeviceInfo(&g->h);
    if (rc != ERROR_SUCCESS) { Log(L"scale: get source %u rc=%ld", s->id, rc); return FALSE; }
    return TRUE;
}

/* step index of the recommended scale, current and maximum; current is unknown (-1) before the first change on
   some systems (curRel holds a placeholder then): the caller falls back to the effective DPI */
static void Steps(const ScaleGet *g, int *rec, int *cur, int *max)
{
    *rec = -g->minRel;
    if (*rec < 0 || *rec >= SCALE_MAXSTEPS) *rec = 0;
    *cur = *rec + g->curRel;
    if (g->curRel < g->minRel || g->curRel > g->maxRel || *cur < 0 || *cur >= SCALE_MAXSTEPS) *cur = -1;
    *max = *rec + g->maxRel;
    if (*max >= SCALE_MAXSTEPS) *max = SCALE_MAXSTEPS - 1;
    if (*max < *rec) *max = *rec;
}

int Scale_Nearest(int pct)
{
    int k, best = 0;
    for (k = 1; k < SCALE_MAXSTEPS; k++)
        if (abs(g_scaleSteps[k] - pct) < abs(g_scaleSteps[best] - pct)) best = k;
    return g_scaleSteps[best];
}

BOOL Scale_Query(int *curPct, int *maxPct)
{
    ScaleSrc src[16];
    int n = Paths(src, 16), i, best = -1, found = 0;
    *curPct = 0; *maxPct = 0;
    for (i = 0; i < n; i++) {
        ScaleGet g;
        int rec, cur, max;
        if (!Get(&src[i], &g)) continue;
        Steps(&g, &rec, &cur, &max);
        found = 1;
        if (g_scaleSteps[max] > best) best = g_scaleSteps[max];
        if (cur >= 0 && (src[i].primary || !*curPct)) *curPct = g_scaleSteps[cur];
    }
    if (!found) return FALSE;
    *maxPct = best;
    if (!*curPct) {     /* no change yet in this session: the effective DPI of the main display */
        POINT pt = { 0, 0 };
        HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        typedef HRESULT (WINAPI *Fn)(HMONITOR, int, UINT *, UINT *);
        Fn f = sh ? (Fn)(void *)GetProcAddress(sh, "GetDpiForMonitor") : NULL;
        UINT dx = 0, dy = 0;
        if (f && SUCCEEDED(f(m, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx) *curPct = Scale_Nearest((int)(dx * 100 / 96));
        else { HDC dc = GetDC(NULL); *curPct = Scale_Nearest(GetDeviceCaps(dc, LOGPIXELSX) * 100 / 96); ReleaseDC(NULL, dc); }
    }
    return TRUE;
}

BOOL Scale_SetAll(int pct, BOOL *changed)
{
    ScaleSrc src[16];
    int n = Paths(src, 16), i, want = -1, k;
    BOOL ok = n > 0;
    *changed = FALSE;
    for (k = 0; k < SCALE_MAXSTEPS; k++) if (g_scaleSteps[k] == pct) want = k;
    if (want < 0) { Log(L"scale: %d%% is not a valid scale", pct); return FALSE; }
    for (i = 0; i < n; i++) {
        ScaleGet g;
        ScaleSetInfo s;
        int rec, cur, max, step;
        LONG rc;
        if (!Get(&src[i], &g)) { ok = FALSE; continue; }
        Steps(&g, &rec, &cur, &max);
        step = want > max ? max : want;
        if (step == cur) { Log(L"scale: source %u already %d%%", src[i].id, g_scaleSteps[step]); continue; }
        ZeroMemory(&s, sizeof(s));
        s.h.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-4;
        s.h.size = sizeof(s);
        s.h.adapterId = src[i].adapter;
        s.h.id = src[i].id;
        s.rel = step - rec;
        rc = DisplayConfigSetDeviceInfo(&s.h);
        Log(L"scale: source %u%s %d%% -> %d%% (asked %d%%, max %d%%) rc=%ld", src[i].id, src[i].primary ? L" (main)" : L"",
            cur >= 0 ? g_scaleSteps[cur] : -1, g_scaleSteps[step], pct, g_scaleSteps[max], rc);
        if (rc == ERROR_SUCCESS) *changed = TRUE; else ok = FALSE;
    }
    return ok;
}
