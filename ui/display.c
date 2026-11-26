/*
 * display.c - MyPE "Display" window (README.md). Own code, plain Win32/GDI, per-monitor DPI aware.
 *
 *   MyPEUI.exe open display [/select N]      (also ms-settings:display, ms-settings:screenrotation)
 *
 * - monitors drawn at their real relative position (1, 2, ...), click = select (or the "Display" list);
 * - mode: Extend / Duplicate / Show only on N (SetDisplayConfig topology);
 * - selected monitor: resolution (modes the driver reports), refresh rate, orientation 0/90/180/270, main display;
 * - scale for all displays, LIVE (scale.c, the per-monitor scale the Settings app sets; VM test 2026-11-25): the list
 *   offers 100 % up to the largest scale EVERY active display accepts (1920x1080: 175 %; one scale on all displays,
 *   the shell is system DPI aware); Apply sets every display, also after a layout change (a display switched on gets
 *   Windows' default), and restarts the shell so the taskbar is drawn sharp at the new DPI ("Restart shell
 *   now" = message PM_MYPE_RESTART_SHELL to the WinXShell desktop window, MyPEStart restarts it). The default at start
 *   is "scale" in display.json (Set-DisplayLayout.ps1 -> MyPEUI.exe scale <percent>);
 * - brightness row only when a controllable brightness exists (brightness.c: WMI laptop panel / DDC/CI monitor);
 * - Apply -> "Keep these display settings?" with automatic revert after REVERT_SECONDS;
 * - "Save as default" writes X:\MyPE\config\display.json (RAM disk: this session only).
 * Every step and failure is logged to X:\MyPE\logs\mypeui.log ("display: ...").
 *
 * Control IDs (stable, used by tests\vm\actions\display.ps1 through CB_SETCURSEL + WM_COMMAND/CBN_SELCHANGE, BM_CLICK):
 *   101 mode  102 display  103 resolution  104 refresh  105 orientation  106 main display (checkbox)
 *   107 brightness (trackbar)  108 scale  109 Restart shell now  110 Save as default  111 Apply  112 Close
 *   120 info text  121 status text  122 scale note  123 brightness value
 *   confirmation window (class MyPE.DisplayConfirm): 201 Keep changes  202 Revert  203 countdown text
 */
#include "mypeui.h"
#include "dispcfg.h"
#include "brightness.h"
#include "scale.h"
#include <commctrl.h>
#include <objbase.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

#define DISP_CLASS    L"MyPE.Display"
#define CONF_CLASS    L"MyPE.DisplayConfirm"
#define DISP_MUTEX    L"Local\\MyPE.Display"
#define REVERT_SECONDS 15

enum {
    IDC_MODE = 101, IDC_MON, IDC_RES, IDC_HZ, IDC_ROT, IDC_PRIMARY, IDC_BRIGHT, IDC_DPI, IDC_RESTARTSHELL, IDC_SAVE,
    IDC_APPLY, IDC_CLOSE,
    IDC_INFO = 120, IDC_STATUS, IDC_DPINOTE, IDC_BRIVAL,
    IDC_L_MODE = 130, IDC_L_MON, IDC_L_RES, IDC_L_HZ, IDC_L_ROT, IDC_L_BRI, IDC_L_DPI,
    IDC_KEEP = 201, IDC_REVERT, IDC_CDTEXT
};
enum { TM_REQUERY = 1, TM_COUNTDOWN = 2 };
#define WM_APP_BRI (WM_APP + 1)       /* brightness worker: probe finished (wParam = targets) */

typedef struct { DWORD w, h, hz; int rot; BOOL primary; } Pend;

static struct {
    HWND hwnd, confirm;
    HINSTANCE hi;
    DcState st;
    Pend pend[DC_MAXMON];
    int sel;
    int modeCur, modePend;      /* 0 extend, 1 duplicate, 2+k only k */
    DWORD dpiCur, dpiPend, dpiSystem;
    double s;
    HFONT font, fontBold;
    RECT map, monRc[DC_MAXMON];
    BOOL monDrawn[DC_MAXMON];
    BOOL busy, confirming;
    int countdown;
    DcSnap snap;
    wchar_t why[64];
    /* brightness */
    BriTarget bri[BRI_MAX];
    int briPct[BRI_MAX];
    volatile LONG nbri;         /* -1 = probing */
    int briOf[DC_MAXMON];       /* monitor -> brightness target, -1 none */
    HANDLE briThread, briEvent;
    volatile LONG briWant, briWantTarget, briWantGen, briQuit, briReprobe;
    LONG briGenUI;              /* probe generation the UI copy (bri/briPct/nbri) belongs to */
    BOOL briShown;
    BOOL selectRotation;
} g;

static int s_scaleMax = 200;   /* largest scale every active display accepts (scale.c), filled at start */
static void ReapplyScale(BOOL *restart, const wchar_t *why);
static void RestartShell(void);

static int S(double v) { return (int)(v * g.s + 0.5); }
static HWND Ctl(int id) { return GetDlgItem(g.hwnd, id); }

static void SetStatus(const wchar_t *fmt, ...)
{
    wchar_t t[600];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(t, _countof(t), _TRUNCATE, fmt, ap);
    va_end(ap);
    SetWindowTextW(Ctl(IDC_STATUS), t);
}

/* ------------------------------------------------------------------------------------------------ */
/* registry: scale (LogPixels)                                                                       */
/* ------------------------------------------------------------------------------------------------ */

static DWORD ReadLogPixels(void)
{
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"LogPixels", RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS || !v) v = 96;
    return v;
}

static DWORD SystemDpi(void)
{
    typedef UINT (WINAPI *Fn)(void);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    Fn f = u ? (Fn)(void *)GetProcAddress(u, "GetDpiForSystem") : NULL;
    UINT d = f ? f() : 0;
    if (!d) { HDC dc = GetDC(NULL); d = (UINT)GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
    return d ? d : 96;
}

/* ------------------------------------------------------------------------------------------------ */
/* display.json                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

static wchar_t *ReadTextFile(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    wchar_t *w;
    int n, off = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) { CloseHandle(h); return NULL; }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    if (got >= 3 && (BYTE)buf[0] == 0xEF && (BYTE)buf[1] == 0xBB && (BYTE)buf[2] == 0xBF) off = 3;
    n = MultiByteToWideChar(CP_UTF8, 0, buf + off, -1, NULL, 0);
    w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, buf + off, -1, w, n);
    free(buf);
    return w;
}

/* minimal reader for our own display.json: "key": "string" / number, searched from p up to end */
static const wchar_t *JFind(const wchar_t *p, const wchar_t *end, const wchar_t *key)
{
    wchar_t pat[64];
    const wchar_t *q;
    size_t len;
    _snwprintf_s(pat, _countof(pat), _TRUNCATE, L"\"%s\"", key);
    len = wcslen(pat);
    for (q = p; q && *q && (!end || q < end); q++) {
        if (!wcsncmp(q, pat, len)) {
            q += len;
            while (*q == L' ' || *q == L'\t' || *q == L'\r' || *q == L'\n') q++;
            if (*q != L':') continue;
            q++;
            while (*q == L' ' || *q == L'\t' || *q == L'\r' || *q == L'\n') q++;
            return q;
        }
    }
    return NULL;
}
static BOOL JStr(const wchar_t *p, const wchar_t *end, const wchar_t *key, wchar_t *out, size_t cch)
{
    const wchar_t *q = JFind(p, end, key);
    size_t i = 0;
    out[0] = 0;
    if (!q || *q != L'"') return FALSE;
    for (q++; *q && *q != L'"' && i + 1 < cch; q++) {
        if (*q == L'\\' && q[1]) q++;
        out[i++] = *q;
    }
    out[i] = 0;
    return TRUE;
}
static long JNum(const wchar_t *p, const wchar_t *end, const wchar_t *key, long def)
{
    const wchar_t *q = JFind(p, end, key);
    return q && (iswdigit(*q) || *q == L'-') ? wcstol(q, NULL, 10) : def;
}
static const wchar_t *JClose(const wchar_t *q, wchar_t open, wchar_t close)
{
    int depth = 0;
    for (; q && *q; q++) {
        if (*q == L'"') { for (q++; *q && *q != L'"'; q++) if (*q == L'\\' && q[1]) q++; if (!*q) return NULL; continue; }
        if (*q == open) depth++;
        else if (*q == close && --depth == 0) return q;
    }
    return NULL;
}

typedef struct { wchar_t id[32]; long w, h; } CfgMon;

/* dpiMode name for display.json (builder registry default): uniform<percent>, also above 200 % */
static const wchar_t *ModeFromDpi(DWORD dpi)
{
    static wchar_t s[24];
    _snwprintf_s(s, _countof(s), _TRUNCATE, L"uniform%lu", (unsigned long)Scale_Nearest((int)(dpi * 100 / 96)));
    return s;
}

static void JEsc(const wchar_t *in, wchar_t *out, size_t cch)
{
    size_t i = 0;
    for (; *in && i + 2 < cch; in++) {
        if (*in == L'"' || *in == L'\\') out[i++] = L'\\';
        out[i++] = *in;
    }
    out[i] = 0;
}

static void SaveDefault(void)
{
    wchar_t *old = ReadTextFile(MYPE_DISPJSON), gpu[128] = L"", txt[4096], esc[160], prim[32] = L"";
    CfgMon cm[DC_MAXMON], ids[DC_MAXMON];
    long fbw = 1920, fbh = 1080;
    int ncm = 0, k, j, nids = 0, len, primIdx = -1;
    const wchar_t *mode;
    char utf8[8192];
    HANDLE h;
    DWORD wr;
    if (old) {
        const wchar_t *a, *b, *fb;
        JStr(old, NULL, L"gpu", gpu, _countof(gpu));
        fb = JFind(old, NULL, L"fallbackResolution");
        if (fb && *fb == L'{') { const wchar_t *fe = JClose(fb, L'{', L'}'); fbw = JNum(fb, fe, L"width", fbw); fbh = JNum(fb, fe, L"height", fbh); }
        a = JFind(old, NULL, L"monitors");
        b = a && *a == L'[' ? JClose(a, L'[', L']') : NULL;
        while (a && b && ncm < DC_MAXMON) {
            const wchar_t *o = wcschr(a, L'{'), *oe;
            if (!o || o > b) break;
            oe = JClose(o, L'{', L'}');
            if (!oe) break;
            JStr(o, oe, L"id", cm[ncm].id, _countof(cm[ncm].id));
            cm[ncm].w = JNum(o, oe, L"width", 0); cm[ncm].h = JNum(o, oe, L"height", 0);
            if (cm[ncm].id[0]) ncm++;
            a = oe + 1;
        }
        free(old);
    }
    /* ids: reuse the configured id of a monitor with the same resolution, else 4K / FHD / M<n> */
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        wchar_t *id = ids[nids].id;
        id[0] = 0;
        if (!mo->active) continue;
        for (j = 0; j < ncm && !id[0]; j++) {
            int used = 0, u;
            for (u = 0; u < nids; u++) if (!wcscmp(ids[u].id, cm[j].id)) used = 1;
            if (!used && (DWORD)cm[j].w == mo->nw && (DWORD)cm[j].h == mo->nh) wcscpy_s(id, 32, cm[j].id);
        }
        if (!id[0]) {
            if (mo->nw == 3840 && mo->nh == 2160) wcscpy_s(id, 32, L"4K");
            else if (mo->nw == 1920 && mo->nh == 1080) wcscpy_s(id, 32, L"FHD");
            for (j = 0; j < nids && id[0]; j++) if (!wcscmp(ids[j].id, id)) id[0] = 0;
            if (!id[0]) _snwprintf_s(id, 32, _TRUNCATE, L"M%d", k + 1);
        }
        ids[nids].w = k;   /* monitor index */
        if (mo->primary && primIdx < 0) { primIdx = k; wcscpy_s(prim, _countof(prim), id); }
        nids++;
    }
    mode = g.st.topo == DC_TOPO_EXTEND ? L"extend" : (g.st.topo == DC_TOPO_CLONE ? L"duplicate" : L"single");
    len = _snwprintf_s(txt, _countof(txt), _TRUNCATE, L"{\r\n");
    if (gpu[0]) { JEsc(gpu, esc, _countof(esc)); len += _snwprintf_s(txt + len, _countof(txt) - len, _TRUNCATE, L"  \"gpu\": \"%s\",\r\n", esc); }
    len += _snwprintf_s(txt + len, _countof(txt) - len, _TRUNCATE, L"  \"mode\": \"%s\",\r\n  \"primary\": \"%s\",\r\n  \"monitors\": [\r\n", mode, prim);
    for (j = 0; j < nids; j++) {
        const DcMonitor *mo = &g.st.m[ids[j].w];
        const DcMonitor *pm = primIdx >= 0 ? &g.st.m[primIdx] : mo;
        const wchar_t *pos = mo == pm ? L"left" : (mo->x < pm->x ? L"left" : L"right");
        if (mo != pm && mo->x == pm->x) pos = mo->y < pm->y ? L"above" : L"below";
        if (mo == pm && nids > 1) {   /* the main display's own side: opposite of the other one */
            const DcMonitor *o = &g.st.m[ids[j == 0 ? 1 : 0].w];
            pos = o->x >= mo->x ? L"left" : L"right";
        }
        JEsc(ids[j].id, esc, _countof(esc));
        len += _snwprintf_s(txt + len, _countof(txt) - len, _TRUNCATE,
                            L"    { \"id\": \"%s\", \"width\": %lu, \"height\": %lu, \"refresh\": %lu, \"position\": \"%s\", \"x\": %d, \"y\": %d, \"rotation\": %d }%s\r\n",
                            esc, mo->nw, mo->nh, mo->hz, pos, mo->x, mo->y, mo->rot, j + 1 < nids ? L"," : L"");
    }
    /* "scale" (percent) is what the start uses; "dpiMode" stays for the builder's registry default */
    len += _snwprintf_s(txt + len, _countof(txt) - len, _TRUNCATE, L"  ],\r\n  \"scale\": %lu,\r\n  \"dpiMode\": \"%s\",\r\n  \"fallbackResolution\": { \"width\": %ld, \"height\": %ld }\r\n}\r\n",
                        g.dpiPend * 100 / 96, ModeFromDpi(g.dpiPend), fbw, fbh);
    (void)len;
    k = WideCharToMultiByte(CP_UTF8, 0, txt, -1, utf8, sizeof(utf8), NULL, NULL);
    h = CreateFileW(MYPE_DISPJSON, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (k <= 1 || h == INVALID_HANDLE_VALUE || !WriteFile(h, utf8, (DWORD)(k - 1), &wr, NULL)) {
        DWORD e = GetLastError();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        Log(L"display: save %s FAILED err=%lu", MYPE_DISPJSON, e);
        SetStatus(L"Could not write %s (error %lu).", MYPE_DISPJSON, e);
        return;
    }
    CloseHandle(h);
    Log(L"display: saved %s: mode=%s primary=%s monitors=%d dpiMode=%s", MYPE_DISPJSON, mode, prim, nids, ModeFromDpi(g.dpiPend));
    SetStatus(L"Saved as default in %s (this session only: X: is a RAM disk).", MYPE_DISPJSON);
}

/* ------------------------------------------------------------------------------------------------ */
/* brightness worker                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

/* The worker owns its own target list (s_wbri: handles, slow calls). The UI thread only gets a copy of the
   descriptive fields through s_briSnap (under s_briCs) in WM_APP_BRI, tagged with a generation number; slider
   requests carry the generation they were made for, so a request from before a re-probe is dropped instead of being
   applied to a different target. */
static BriTarget s_wbri[BRI_MAX];
static int s_wn;
static CRITICAL_SECTION s_briCs;
static BriTarget s_briSnap[BRI_MAX];
static int s_briSnapPct[BRI_MAX], s_briSnapN = -1;
static LONG s_briSnapGen;

static DWORD WINAPI BriWorker(LPVOID p)
{
    int n, i;
    LONG gen = 0;
    (void)p;
    for (;;) {
        int pct[BRI_MAX];
        Bri_Free(s_wbri, s_wn);
        s_wn = 0;
        n = Bri_Probe(s_wbri, BRI_MAX);
        for (i = 0; i < n; i++) pct[i] = Bri_GetPercent(&s_wbri[i]);
        s_wn = n;
        gen++;
        EnterCriticalSection(&s_briCs);
        for (i = 0; i < n; i++) { s_briSnap[i] = s_wbri[i]; s_briSnapPct[i] = pct[i]; }
        s_briSnapN = n;
        s_briSnapGen = gen;
        LeaveCriticalSection(&s_briCs);
        PostMessageW(g.hwnd, WM_APP_BRI, (WPARAM)gen, 0);
        for (;;) {
            LONG v, t;
            WaitForSingleObject(g.briEvent, INFINITE);
            if (g.briQuit) { Bri_Free(s_wbri, s_wn); s_wn = 0; CoUninitialize(); return 0; }
            if (InterlockedExchange(&g.briReprobe, 0)) break;
            v = InterlockedExchange(&g.briWant, -1);
            t = g.briWantTarget;
            if (v >= 0 && g.briWantGen != gen) { Log(L"display: brightness request for an old target list dropped"); continue; }
            if (v >= 0 && t >= 0 && t < s_wn) {
                BOOL ok = Bri_SetPercent(&s_wbri[t], (int)v);
                int back = Bri_GetPercent(&s_wbri[t]);
                Log(L"display: brightness %s '%s' set %ld%% -> %s, read back %d%%", Bri_KindName(s_wbri[t].kind), s_wbri[t].desc, v, ok ? L"ok" : L"FAILED", back);
            }
        }
    }
}

/* UI thread: take the worker's latest list (WM_APP_BRI) */
static void TakeBrightness(void)
{
    int i;
    EnterCriticalSection(&s_briCs);
    g.nbri = s_briSnapN;
    for (i = 0; i < s_briSnapN; i++) { g.bri[i] = s_briSnap[i]; g.briPct[i] = s_briSnapPct[i]; }
    g.briGenUI = s_briSnapGen;
    LeaveCriticalSection(&s_briCs);
}

static void MapBrightness(void)
{
    int k, t, internal = 0;
    for (k = 0; k < DC_MAXMON; k++) g.briOf[k] = -1;
    if (g.nbri <= 0) return;
    for (k = 0; k < g.st.n; k++) if (g.st.m[k].internal) internal = 1;
    for (t = 0; t < g.nbri; t++) {
        for (k = 0; k < g.st.n; k++) {
            const DcMonitor *mo = &g.st.m[k];
            if (g.briOf[k] >= 0) continue;
            if (g.bri[t].kind == BRI_DDC && mo->active && !_wcsicmp(mo->gdi, g.bri[t].gdi)) g.briOf[k] = t;
            if (g.bri[t].kind == BRI_WMI && (internal ? mo->internal : mo->primary)) g.briOf[k] = t;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* state -> controls                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

static int ModeOfState(const DcState *s)
{
    if (s->topo == DC_TOPO_CLONE) return 1;
    if (s->topo == DC_TOPO_ONLY) return 2 + s->only;
    return 0;
}

static void ResetPending(void)
{
    int k;
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        g.pend[k].w = mo->nw; g.pend[k].h = mo->nh; g.pend[k].hz = mo->hz; g.pend[k].rot = mo->rot; g.pend[k].primary = mo->primary;
    }
    g.modeCur = g.modePend = ModeOfState(&g.st);
}

static BOOL MonitorDirty(int k)
{
    const DcMonitor *mo = &g.st.m[k];
    const Pend *p = &g.pend[k];
    return mo->active && (p->w != mo->nw || p->h != mo->nh || p->hz != mo->hz || p->rot != mo->rot || p->primary != mo->primary);
}
static BOOL DisplayDirty(void)
{
    int k;
    if (g.modePend != g.modeCur) return TRUE;
    for (k = 0; k < g.st.n; k++) if (MonitorDirty(k)) return TRUE;
    return FALSE;
}

static void CbReset(int id) { SendMessageW(Ctl(id), CB_RESETCONTENT, 0, 0); }
static int CbAdd(int id, const wchar_t *t, LPARAM data)
{
    int i = (int)SendMessageW(Ctl(id), CB_ADDSTRING, 0, (LPARAM)t);
    if (i >= 0) SendMessageW(Ctl(id), CB_SETITEMDATA, (WPARAM)i, data);
    return i;
}
static LPARAM CbData(int id)
{
    int i = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
    return i >= 0 ? (LPARAM)SendMessageW(Ctl(id), CB_GETITEMDATA, (WPARAM)i, 0) : -1;
}
static void CbSelData(int id, LPARAM data)
{
    int i, n = (int)SendMessageW(Ctl(id), CB_GETCOUNT, 0, 0);
    for (i = 0; i < n; i++) if ((LPARAM)SendMessageW(Ctl(id), CB_GETITEMDATA, (WPARAM)i, 0) == data) { SendMessageW(Ctl(id), CB_SETCURSEL, (WPARAM)i, 0); return; }
    if (n > 0) SendMessageW(Ctl(id), CB_SETCURSEL, 0, 0);
}

static void FillHz(void)
{
    const DcMonitor *mo = &g.st.m[g.sel];
    Pend *p = &g.pend[g.sel];
    wchar_t t[32];
    int i, j, found = 0;
    CbReset(IDC_HZ);
    for (i = 0; i < mo->nmodes; i++) {
        const DcMode *md = &mo->modes[i];
        if (md->w != p->w || md->h != p->h) continue;
        for (j = 0; j < i; j++) if (mo->modes[j].w == md->w && mo->modes[j].h == md->h && mo->modes[j].hz == md->hz) break;
        if (j < i) continue;
        if (md->hz <= 1) wcscpy_s(t, _countof(t), L"Default");
        else _snwprintf_s(t, _countof(t), _TRUNCATE, L"%lu Hz", md->hz);
        CbAdd(IDC_HZ, t, (LPARAM)md->hz);
        if (md->hz == p->hz) found = 1;
    }
    if (!found) {
        if (SendMessageW(Ctl(IDC_HZ), CB_GETCOUNT, 0, 0) > 0) p->hz = (DWORD)SendMessageW(Ctl(IDC_HZ), CB_GETITEMDATA, 0, 0);
        else if (mo->active) { _snwprintf_s(t, _countof(t), _TRUNCATE, L"%lu Hz", p->hz); CbAdd(IDC_HZ, t, (LPARAM)p->hz); }
    }
    CbSelData(IDC_HZ, (LPARAM)p->hz);
}

static void FillMonitorControls(void)
{
    const DcMonitor *mo = &g.st.m[g.sel];
    Pend *p = &g.pend[g.sel];
    wchar_t t[96];
    int i, j, curFound = 0;
    static const wchar_t *rots[] = { L"Landscape (0\u00B0)", L"Portrait (90\u00B0)", L"Landscape, flipped (180\u00B0)", L"Portrait, flipped (270\u00B0)" };
    CbReset(IDC_RES);
    for (i = 0; i < mo->nmodes; i++) {
        const DcMode *md = &mo->modes[i];
        for (j = 0; j < i; j++) if (mo->modes[j].w == md->w && mo->modes[j].h == md->h) break;
        if (j < i) continue;
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%lu \u00D7 %lu%s", md->w, md->h, (md->w == mo->prefW && md->h == mo->prefH) ? L" (recommended)" : L"");
        CbAdd(IDC_RES, t, (LPARAM)MAKELONG(md->w, md->h));
        if (md->w == p->w && md->h == p->h) curFound = 1;
    }
    if (mo->active && !curFound) {   /* current mode not in the list (driver quirk): still show it */
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%lu \u00D7 %lu", p->w, p->h);
        CbAdd(IDC_RES, t, (LPARAM)MAKELONG(p->w, p->h));
    }
    CbSelData(IDC_RES, (LPARAM)MAKELONG(p->w, p->h));
    FillHz();
    CbReset(IDC_ROT);
    for (i = 0; i < 4; i++) CbAdd(IDC_ROT, rots[i], i * 90);
    CbSelData(IDC_ROT, p->rot);
    SendMessageW(Ctl(IDC_PRIMARY), BM_SETCHECK, p->primary ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void UpdateEnable(void)
{
    const DcMonitor *mo = &g.st.m[g.sel];
    BOOL idle = !g.busy && !g.confirming;
    BOOL monEdit = idle && g.st.n > 0 && mo->active && g.modePend == g.modeCur;
    BOOL dirty = DisplayDirty() || g.dpiPend != g.dpiCur;
    EnableWindow(Ctl(IDC_MODE), idle && g.st.n >= 2);
    EnableWindow(Ctl(IDC_MON), idle && g.st.n >= 2);
    EnableWindow(Ctl(IDC_RES), monEdit && mo->nmodes > 0);
    EnableWindow(Ctl(IDC_HZ), monEdit && SendMessageW(Ctl(IDC_HZ), CB_GETCOUNT, 0, 0) > 1);
    EnableWindow(Ctl(IDC_ROT), monEdit);
    EnableWindow(Ctl(IDC_PRIMARY), monEdit && g.st.topo == DC_TOPO_EXTEND && !mo->primary);
    EnableWindow(Ctl(IDC_BRIGHT), idle);
    EnableWindow(Ctl(IDC_DPI), idle);
    EnableWindow(Ctl(IDC_APPLY), idle && dirty);
    EnableWindow(Ctl(IDC_SAVE), idle && !DisplayDirty() && g.st.n > 0);
    EnableWindow(Ctl(IDC_RESTARTSHELL), idle);
    EnableWindow(Ctl(IDC_CLOSE), idle);
}

static void UpdateDpiNote(void)
{
    wchar_t t[300];
    /* live scale (scale.c): applied at once, the shell restarts to draw the taskbar at the new DPI */
    if (g.dpiPend != g.dpiCur)
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"Apply changes all displays to %lu%% now (the same scale on every display); the taskbar restarts. Open windows of older programs may look blurry until they are reopened.", g.dpiPend * 100 / 96);
    else
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"Same scale on all displays. Default at start: \"scale\" in MyPE\\config\\display.json (on the boot drive or in the image).");
    SetWindowTextW(Ctl(IDC_DPINOTE), t);
}

static void Layout(void);

static void UpdateBrightnessRow(void)
{
    int t = g.st.n > 0 ? g.briOf[g.sel] : -1;
    BOOL show = t >= 0;
    if (show) {
        wchar_t v[16];
        int pct = g.briPct[t] >= 0 ? g.briPct[t] : 50;
        SendMessageW(Ctl(IDC_BRIGHT), TBM_SETPOS, TRUE, pct);
        _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d%%", pct);
        SetWindowTextW(Ctl(IDC_BRIVAL), v);
    }
    if (show != g.briShown) {
        g.briShown = show;
        ShowWindow(Ctl(IDC_L_BRI), show ? SW_SHOW : SW_HIDE);
        ShowWindow(Ctl(IDC_BRIGHT), show ? SW_SHOW : SW_HIDE);
        ShowWindow(Ctl(IDC_BRIVAL), show ? SW_SHOW : SW_HIDE);
        Layout();
    }
}

static void RefreshControls(void)
{
    wchar_t t[300];
    int k, mon;
    const wchar_t *adapter = g.st.n > 0 ? g.st.m[0].adapterName : L"";
    for (k = 0; k < g.st.n; k++) if (g.st.m[k].active && g.st.m[k].adapterName[0]) { adapter = g.st.m[k].adapterName; break; }
    if (g.st.n == 0) wcscpy_s(t, _countof(t), L"No display detected.");
    else _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d monitor%s detected%s%s", g.st.n, g.st.n == 1 ? L"" : L"s",
                      adapter[0] ? L" \u2014 " : L"", adapter);
    SetWindowTextW(Ctl(IDC_INFO), t);

    CbReset(IDC_MODE);
    if (g.st.n >= 2) {
        CbAdd(IDC_MODE, L"Extend these displays", 0);
        CbAdd(IDC_MODE, L"Duplicate these displays", 1);
        for (k = 0; k < g.st.n; k++) { _snwprintf_s(t, _countof(t), _TRUNCATE, L"Show only on %d", k + 1); CbAdd(IDC_MODE, t, 2 + k); }
        CbSelData(IDC_MODE, g.modePend);
    } else {
        CbAdd(IDC_MODE, g.st.n == 1 ? L"Single display" : L"No display", 0);
        SendMessageW(Ctl(IDC_MODE), CB_SETCURSEL, 0, 0);
    }
    CbReset(IDC_MON);
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d. %s%s%s", k + 1, mo->name, mo->primary && mo->active ? L" (main)" : L"", mo->active ? L"" : L" (off)");
        CbAdd(IDC_MON, t, k);
    }
    mon = g.st.n ? g.sel : -1;
    if (mon >= 0) {
        CbSelData(IDC_MON, mon);
        FillMonitorControls();
    } else { CbReset(IDC_RES); CbReset(IDC_HZ); CbReset(IDC_ROT); }
    CbSelData(IDC_DPI, (LPARAM)g.dpiPend);
    UpdateDpiNote();
    UpdateBrightnessRow();
    UpdateEnable();
    InvalidateRect(g.hwnd, &g.map, FALSE);
}

static void Requery(const wchar_t *why)
{
    wchar_t d[1400];
    if (!Dc_Query(&g.st)) ZeroMemory(&g.st, sizeof(g.st));
    if (g.sel >= g.st.n) g.sel = 0;
    ResetPending();
    MapBrightness();
    Dc_Describe(&g.st, d, _countof(d));
    Log(L"display: state (%s): %s", why, d);
    RefreshControls();
}

/* ------------------------------------------------------------------------------------------------ */
/* monitor map                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

static void PaintMap(HDC dc)
{
    RECT r = g.map, vr[DC_MAXMON];
    int W = r.right - r.left, H = r.bottom - r.top, k, j;
    long minx = 0, miny = 0, maxx = 0, maxy = 0;
    BOOL any = FALSE;
    double sc;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ ob = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(232, 234, 238));
    RECT all;
    SetRect(&all, 0, 0, W, H);
    FillRect(mem, &all, bg);
    DeleteObject(bg);
    ZeroMemory(g.monDrawn, sizeof(g.monDrawn));

    /* virtual layout: active monitors at their desktop position, inactive ones to the right */
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        if (!mo->active || !mo->w || !mo->h) continue;
        SetRect(&vr[k], mo->x, mo->y, mo->x + (int)mo->w, mo->y + (int)mo->h);
        if (!any) { minx = vr[k].left; miny = vr[k].top; maxx = vr[k].right; maxy = vr[k].bottom; any = TRUE; }
        minx = min(minx, vr[k].left); miny = min(miny, vr[k].top); maxx = max(maxx, vr[k].right); maxy = max(maxy, vr[k].bottom);
    }
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        long w, h;
        if (mo->active && mo->w && mo->h) continue;
        w = mo->prefW ? (long)mo->prefW : 1920; h = mo->prefH ? (long)mo->prefH : 1080;
        if (!any) { minx = 0; miny = 0; maxx = 0; maxy = h; any = TRUE; }
        SetRect(&vr[k], maxx + (any ? (LONG)(w / 12) : 0), miny, maxx + (LONG)(w / 12) + w, miny + h);
        maxx = vr[k].right; maxy = max(maxy, vr[k].bottom);
    }
    if (any && maxx > minx && maxy > miny) {
        double mx = 14 * g.s, my = 12 * g.s;
        sc = min((W - 2 * mx) / (double)(maxx - minx), (H - 2 * my) / (double)(maxy - miny));
        for (k = 0; k < g.st.n; k++) {
            const DcMonitor *mo = &g.st.m[k];
            double ox = (W - (maxx - minx) * sc) / 2, oy = (H - (maxy - miny) * sc) / 2;
            RECT d;
            HBRUSH br;
            HPEN pen;
            HGDIOBJ op, obr, of;
            wchar_t num[16], sub[48];
            int fh, clone = 0;
            BOOL sel = k == g.sel;
            d.left = (LONG)(ox + (vr[k].left - minx) * sc) + S(2); d.top = (LONG)(oy + (vr[k].top - miny) * sc) + S(2);
            d.right = (LONG)(ox + (vr[k].right - minx) * sc) - S(2); d.bottom = (LONG)(oy + (vr[k].bottom - miny) * sc) - S(2);
            /* duplicate: same rectangle for several monitors, drawn once with all numbers */
            for (j = 0; j < k; j++) if (g.st.m[j].active && mo->active && EqualRect(&vr[j], &vr[k])) clone = 1;
            if (clone) { OffsetRect(&d, g.map.left, g.map.top); g.monRc[k] = d; g.monDrawn[k] = TRUE; continue; }
            num[0] = 0;
            for (j = k; j < g.st.n; j++) {
                if (j == k || (g.st.m[j].active && mo->active && EqualRect(&vr[j], &vr[k]))) {
                    size_t l = wcslen(num);
                    _snwprintf_s(num + l, _countof(num) - l, _TRUNCATE, l ? L"|%d" : L"%d", j + 1);
                    if (j == g.sel) sel = TRUE;
                }
            }
            br = CreateSolidBrush(!mo->active ? RGB(210, 212, 216) : (sel ? RGB(0, 103, 192) : RGB(160, 166, 176)));
            pen = CreatePen(PS_SOLID, max(1, S(sel ? 2 : 1)), sel ? RGB(0, 60, 120) : RGB(90, 94, 100));
            obr = SelectObject(mem, br); op = SelectObject(mem, pen);
            Rectangle(mem, d.left, d.top, d.right, d.bottom);
            SelectObject(mem, obr); SelectObject(mem, op);
            DeleteObject(br); DeleteObject(pen);
            if (!mo->active) {
                HBRUSH hb = CreateHatchBrush(HS_BDIAGONAL, RGB(180, 182, 186));
                RECT in = d;
                InflateRect(&in, -S(2), -S(2));
                SetBkMode(mem, TRANSPARENT);
                FillRect(mem, &in, hb);
                DeleteObject(hb);
            }
            fh = (int)min((d.bottom - d.top) * 0.45, 40 * g.s);
            if (fh < S(10)) fh = S(10);
            {
                HFONT f = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                RECT tr = d;
                of = SelectObject(mem, f);
                SetBkMode(mem, TRANSPARENT);
                SetTextColor(mem, mo->active ? RGB(255, 255, 255) : RGB(80, 80, 80));
                if (d.bottom - d.top > fh + S(30)) tr.bottom -= S(14);
                DrawTextW(mem, num, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(mem, of);
                DeleteObject(f);
            }
            if (d.bottom - d.top > fh + S(30)) {
                RECT sr = d;
                if (mo->active) _snwprintf_s(sub, _countof(sub), _TRUNCATE, L"%lu \u00D7 %lu%s", mo->nw, mo->nh, mo->primary ? L"  main" : L"");
                else wcscpy_s(sub, _countof(sub), L"off");
                sr.top = sr.bottom - S(26);
                of = SelectObject(mem, g.font);
                DrawTextW(mem, sub, -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(mem, of);
            }
            OffsetRect(&d, g.map.left, g.map.top);
            g.monRc[k] = d; g.monDrawn[k] = TRUE;
        }
    } else {
        RECT tr = all;
        HGDIOBJ of = SelectObject(mem, g.font);
        SetBkMode(mem, TRANSPARENT);
        DrawTextW(mem, L"No display information (QueryDisplayConfig failed)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, of);
    }
    BitBlt(dc, r.left, r.top, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void SelectMonitor(int k, const wchar_t *how)
{
    if (k < 0 || k >= g.st.n) return;
    g.sel = k;
    Log(L"display: selected monitor %d (%s)", k + 1, how);
    CbSelData(IDC_MON, k);
    FillMonitorControls();
    UpdateBrightnessRow();
    UpdateEnable();
    InvalidateRect(g.hwnd, &g.map, FALSE);
}

static void MapClick(int x, int y)
{
    POINT pt;
    int k, first = -1, afterSel = -1;
    pt.x = x; pt.y = y;
    for (k = 0; k < g.st.n; k++) {
        if (!g.monDrawn[k] || !PtInRect(&g.monRc[k], pt)) continue;
        if (first < 0) first = k;
        if (k > g.sel && afterSel < 0) afterSel = k;
    }
    if (first < 0) return;
    /* overlapping (duplicate): each click selects the next one */
    if (PtInRect(&g.monRc[g.sel], pt) && afterSel >= 0) SelectMonitor(afterSel, L"click");
    else SelectMonitor(first, L"click");
}

/* ------------------------------------------------------------------------------------------------ */
/* apply / confirm / revert                                                                          */
/* ------------------------------------------------------------------------------------------------ */

static void EnsureVisible(HWND h)
{
    if (!MonitorFromWindow(h, MONITOR_DEFAULTTONULL)) {
        MONITORINFO mi;
        RECT wr;
        POINT o = {0, 0};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
        GetWindowRect(h, &wr);
        SetWindowPos(h, NULL, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (wr.right - wr.left)) / 2,
                     mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        Log(L"display: window moved onto the main display");
    }
}

static void FinishConfirm(BOOL keep, const wchar_t *how)
{
    wchar_t d[1400];
    if (!g.confirming) return;
    g.confirming = FALSE;
    KillTimer(g.confirm, TM_COUNTDOWN);
    EnableWindow(g.hwnd, TRUE);
    if (g.confirm) { DestroyWindow(g.confirm); g.confirm = NULL; }
    if (keep) {
        Log(L"display: changes kept (%s)", how);
        Dc_FreeSnap(&g.snap);
        Requery(L"kept");
        SetStatus(L"Display settings kept.");
    } else {
        LONG rc;
        BOOL restart = FALSE;
        g.busy = TRUE;
        rc = Dc_Restore(&g.snap);
        Dc_FreeSnap(&g.snap);
        g.busy = FALSE;
        Sleep(300);
        ReapplyScale(&restart, L"after the revert");   /* a display switched back on gets Windows' default scale */
        if (restart) RestartShell();
        Requery(L"reverted");
        Dc_Describe(&g.st, d, _countof(d));
        Log(L"display: REVERTED (%s) rc=%ld: %s", how, rc, d);
        if (rc == ERROR_SUCCESS) SetStatus(L"Previous display settings restored (%s).", how);
        else SetStatus(L"Could not fully restore the previous settings (error %ld). See X:\\MyPE\\logs\\mypeui.log.", rc);
    }
    EnsureVisible(g.hwnd);
    SetForegroundWindow(g.hwnd);
}

static void ConfirmText(void)
{
    wchar_t t[200];
    _snwprintf_s(t, _countof(t), _TRUNCATE, L"Do you want to keep these display settings?\r\n\r\nReverting to the previous settings in %d second%s.",
                 g.countdown, g.countdown == 1 ? L"" : L"s");
    SetDlgItemTextW(g.confirm, IDC_CDTEXT, t);
}

static LRESULT CALLBACK ConfirmProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        if (wp == TM_COUNTDOWN && g.confirming) {
            if (--g.countdown <= 0) { FinishConfirm(FALSE, L"no confirmation within 15 s"); return 0; }
            ConfirmText();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_KEEP) FinishConfirm(TRUE, L"Keep changes");
        else if (LOWORD(wp) == IDC_REVERT || LOWORD(wp) == IDCANCEL) FinishConfirm(FALSE, L"Revert");
        return 0;
    case WM_CLOSE:
        FinishConfirm(FALSE, L"confirmation closed");
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void ShowConfirm(void)
{
    MONITORINFO mi;
    POINT o = {0, 0};
    RECT rc;
    int cw, ch, x, y;
    DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    HWND b;
    double s;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi);
    s = g.s;
    cw = (int)(420 * s); ch = (int)(150 * s);
    SetRect(&rc, 0, 0, cw, ch);
    AdjustWindowRectEx(&rc, st, FALSE, ex);
    x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2;
    y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2;
    g.confirm = CreateWindowExW(ex, CONF_CLASS, L"Display settings", st, x, y, rc.right - rc.left, rc.bottom - rc.top, g.hwnd, NULL, g.hi, NULL);
    if (!g.confirm) {   /* no confirmation window = no way to confirm: revert right away */
        Log(L"display: confirmation window failed err=%lu - reverting", GetLastError());
        g.confirming = TRUE;
        FinishConfirm(FALSE, L"confirmation window failed");
        return;
    }
    b = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, (int)(16 * s), (int)(14 * s), cw - (int)(32 * s), (int)(70 * s), g.confirm, (HMENU)(INT_PTR)IDC_CDTEXT, g.hi, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
    b = CreateWindowExW(0, L"BUTTON", L"Keep changes", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, cw - (int)(268 * s), ch - (int)(44 * s), (int)(124 * s), (int)(30 * s), g.confirm, (HMENU)(INT_PTR)IDC_KEEP, g.hi, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
    b = CreateWindowExW(0, L"BUTTON", L"Revert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, cw - (int)(136 * s), ch - (int)(44 * s), (int)(120 * s), (int)(30 * s), g.confirm, (HMENU)(INT_PTR)IDC_REVERT, g.hi, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
    g.confirming = TRUE;
    g.countdown = REVERT_SECONDS;
    ConfirmText();
    EnableWindow(g.hwnd, FALSE);
    ShowWindow(g.confirm, SW_SHOW);
    ForceForeground(g.confirm);
    SetFocus(b);
    SetTimer(g.confirm, TM_COUNTDOWN, 1000, NULL);
    Log(L"display: confirmation shown (revert in %d s) at %d,%d", REVERT_SECONDS, x, y);
}

/* GDI set for the pending per-monitor changes; positions recomputed side by side (or stacked), main display at 0,0 */
static int BuildSet(DcSet *set)
{
    int order[DC_MAXMON], n = 0, k, j, prim = -1, vertical = 0;
    long cur = 0, dx, dy;
    for (k = 0; k < g.st.n; k++) {
        const DcMonitor *mo = &g.st.m[k];
        int dup = 0;
        if (!mo->active || !mo->gdi[0]) continue;
        for (j = 0; j < n; j++) if (!_wcsicmp(g.st.m[order[j]].gdi, mo->gdi)) {
            dup = 1;
            if (k == g.sel) order[j] = k;   /* duplicate: the selected monitor's settings for the shared source */
        }
        if (!dup) order[n++] = k;
    }
    /* order by current position */
    if (n == 2) {
        const DcMonitor *a = &g.st.m[order[0]], *b = &g.st.m[order[1]];
        vertical = (a->y + (long)a->h <= b->y) || (b->y + (long)b->h <= a->y);
    }
    for (k = 1; k < n; k++) for (j = k; j > 0; j--) {
        const DcMonitor *a = &g.st.m[order[j - 1]], *b = &g.st.m[order[j]];
        if (vertical ? (b->y < a->y) : (b->x < a->x)) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; } else break;
    }
    for (j = 0; j < n; j++) {
        const Pend *p = &g.pend[order[j]];
        DWORD rw = (p->rot == 90 || p->rot == 270) ? p->h : p->w, rh = (p->rot == 90 || p->rot == 270) ? p->w : p->h;
        set[j].gdi = g.st.m[order[j]].gdi;
        set[j].w = p->w; set[j].h = p->h; set[j].hz = p->hz; set[j].rot = p->rot;
        set[j].primary = p->primary;
        if (n == 1) { set[j].x = 0; set[j].y = 0; }
        else if (vertical) { set[j].x = 0; set[j].y = cur; cur += (long)rh; }
        else { set[j].x = cur; set[j].y = 0; cur += (long)rw; }
        if (p->primary && prim < 0) prim = j;
    }
    if (prim < 0) prim = 0;
    dx = set[prim].x; dy = set[prim].y;
    for (j = 0; j < n; j++) { set[j].x -= dx; set[j].y -= dy; set[j].primary = j == prim; }
    return n;
}

static void RestartShell(void);

/* scale combo: 100 % .. the largest scale every display accepts (scale.c); the scale in use is always listed, also
   when it is above that limit (e.g. a display with a lower maximum was just switched on). Item data = dpi. */
static void FillScaleList(void)
{
    HWND cb = Ctl(IDC_DPI);
    int k, cur = 0, max = 0;
    BOOL curListed = FALSE;
    if (Scale_Query(&cur, &max)) s_scaleMax = max;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (k = 0; k < SCALE_MAXSTEPS && g_scaleSteps[k] <= s_scaleMax; k++) {
        wchar_t t[32];
        DWORD dpi = (DWORD)(g_scaleSteps[k] * 96 / 100);
        if (dpi == g.dpiCur) curListed = TRUE;
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%d%%%s", g_scaleSteps[k], dpi == g.dpiCur ? L" (current)" : L"");
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t), (LPARAM)dpi);
    }
    if (!curListed) {
        wchar_t t[40];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"%lu%% (current)", g.dpiCur * 100 / 96);
        SendMessageW(cb, CB_SETITEMDATA, (WPARAM)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t), (LPARAM)g.dpiCur);
    }
    CbSelData(IDC_DPI, (LPARAM)g.dpiPend);
}

/* the scale in use after a change: what Windows reports (a partial failure leaves the displays where they are) */
static void ReadScaleBack(void)
{
    int cur = 0, max = 0;
    if (Scale_Query(&cur, &max) && cur > 0) { g.dpiCur = (DWORD)(cur * 96 / 100); s_scaleMax = max; }
}

/* a display switched on or restored gets Windows' default scale: put the one scale back on every display */
static void ReapplyScale(BOOL *restart, const wchar_t *why)
{
    BOOL ch = FALSE;
    if (Scale_SetAll((int)(g.dpiCur * 100 / 96), &ch) && ch) {
        Log(L"display: scale %lu%% re-applied (%s)", g.dpiCur * 100 / 96, why);
        *restart = TRUE;
    }
    ReadScaleBack();
    g.dpiPend = g.dpiCur;
}

static void Apply(void)
{
    wchar_t err[200], d[1400];
    LONG rc = 0;
    BOOL ok, restart = FALSE;   /* the shell is restarted ONCE, at the end, if any scale changed */
    if (g.busy || g.confirming) return;
    if (g.dpiPend != g.dpiCur) {
        BOOL changed = FALSE;
        int pct = (int)(g.dpiPend * 100 / 96);
        BOOL all = Scale_SetAll(pct, &changed);
        ReadScaleBack();
        g.dpiPend = g.dpiCur;
        restart = changed;
        Log(L"display: scale %d%% applied live (all=%d changed=%d, now %lu%%)", pct, all, changed, g.dpiCur * 100 / 96);
        if (all) SetStatus(L"Scale %lu%% applied.%s", g.dpiCur * 100 / 96, changed ? L" The taskbar restarts." : L"");
        else SetStatus(L"The scale could not be changed on every display (see the log).");
        FillScaleList();
        UpdateDpiNote();
    }
    if (!DisplayDirty()) { if (restart) RestartShell(); UpdateEnable(); return; }
    g.busy = TRUE;
    UpdateEnable();
    Dc_Snapshot(&g.snap);
    Dc_Describe(&g.st, d, _countof(d));
    Log(L"display: apply from: %s", d);
    err[0] = 0;
    if (g.modePend != g.modeCur) {
        int topo = g.modePend == 0 ? DC_TOPO_EXTEND : (g.modePend == 1 ? DC_TOPO_CLONE : DC_TOPO_ONLY);
        Log(L"display: apply mode %s%s", Dc_TopoName(topo), topo == DC_TOPO_ONLY ? L" (one monitor)" : L"");
        rc = Dc_SetTopology(&g.st, topo, g.modePend - 2);
        ok = rc == ERROR_SUCCESS;
        if (!ok) _snwprintf_s(err, _countof(err), _TRUNCATE, L"SetDisplayConfig error %ld", rc);
    } else {
        DcSet set[DC_MAXMON];
        int n = BuildSet(set);
        rc = Dc_ApplyGdi(set, n, err, _countof(err));
        ok = rc == DISP_CHANGE_SUCCESSFUL;
    }
    g.busy = FALSE;
    if (!ok) {
        LONG r2 = Dc_Restore(&g.snap);
        Dc_FreeSnap(&g.snap);
        Log(L"display: apply FAILED (%s); restore rc=%ld", err, r2);
        Sleep(300);
        ReapplyScale(&restart, L"after a refused layout change");
        if (restart) RestartShell();
        Requery(L"apply failed");
        SetStatus(L"The change was refused: %s. Nothing was changed.", err);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    Sleep(500);   /* let the mode settle before reading it back */
    ReapplyScale(&restart, L"after the layout change");
    if (restart) RestartShell();
    Requery(L"applied");
    EnsureVisible(g.hwnd);
    SetStatus(L"Applied. Confirm within %d seconds or the previous settings come back.", REVERT_SECONDS);
    ShowConfirm();
}

static void RestartShell(void)
{
    HWND desk = FindWindowW(L"Progman", NULL);
    DWORD pid = 0;
    if (!desk) { Log(L"display: restart shell: no Progman window"); SetStatus(L"Shell window not found: restart the shell from the desktop menu."); return; }
    GetWindowThreadProcessId(desk, &pid);
    Log(L"display: restart shell requested (Progman %p, pid %lu)", (void *)desk, pid);
    if (!PostMessageW(desk, MYPE_PM_RESTART_SHELL, 0, 0)) { Log(L"display: PostMessage failed err=%lu", GetLastError()); SetStatus(L"Could not ask the shell to restart (error %lu).", GetLastError()); return; }
    SetStatus(L"The shell is restarting (the taskbar disappears for a few seconds).");
}

/* ------------------------------------------------------------------------------------------------ */
/* window                                                                                            */
/* ------------------------------------------------------------------------------------------------ */

static void CreateFonts(void)
{
    if (g.font) DeleteObject(g.font);
    if (g.fontBold) DeleteObject(g.fontBold);
    g.font = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g.fontBold = CreateFontW(-S(12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static BOOL CALLBACK SetFontCb(HWND c, LPARAM lp) { SendMessageW(c, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }

static void Place(int id, int x, int y, int w, int h) { SetWindowPos(Ctl(id), NULL, S(x), S(y), S(w), S(h), SWP_NOZORDER | SWP_NOACTIVATE); }

static int ClientWidth(void) { return 580; }

static void Layout(void)
{
    const int M = 16, LW = 150, CX = M + LW + 8, RH = 34;
    int W = ClientWidth(), y, cw;
    RECT wr, cr;
    SetRect(&g.map, S(M), S(12), S(W - M), S(12 + 170));
    y = 190;
    Place(IDC_INFO, M, y, W - 2 * M, 20); y += 30;
    Place(IDC_L_MODE, M, y + 4, LW, 20); Place(IDC_MODE, CX, y, 260, 300); y += RH;
    Place(IDC_L_MON, M, y + 4, LW, 20); Place(IDC_MON, CX, y, W - M - CX, 300); y += RH;
    Place(IDC_L_RES, M, y + 4, LW, 20); Place(IDC_RES, CX, y, 260, 400); y += RH;
    Place(IDC_L_HZ, M, y + 4, LW, 20); Place(IDC_HZ, CX, y, 160, 300); y += RH;
    Place(IDC_L_ROT, M, y + 4, LW, 20); Place(IDC_ROT, CX, y, 260, 300); y += RH;
    Place(IDC_PRIMARY, CX, y, W - M - CX, 24); y += RH;
    if (g.briShown) {
        Place(IDC_L_BRI, M, y + 6, LW, 20); Place(IDC_BRIGHT, CX - 4, y, 300, 30); Place(IDC_BRIVAL, CX + 304, y + 6, 60, 20); y += RH + 4;
    }
    Place(IDC_L_DPI, M, y + 4, LW, 20); Place(IDC_DPI, CX, y, 180, 300); Place(IDC_RESTARTSHELL, CX + 192, y - 1, W - M - CX - 192, 28); y += RH;
    Place(IDC_DPINOTE, CX, y, W - M - CX, 36); y += 44;
    Place(IDC_STATUS, M, y, W - 2 * M, 36); y += 44;
    cw = 130;
    Place(IDC_SAVE, W - M - 3 * cw - 16, y, cw, 30);
    Place(IDC_APPLY, W - M - 2 * cw - 8, y, cw, 30);
    Place(IDC_CLOSE, W - M - cw, y, cw, 30);
    y += 30 + 14;
    /* window height follows the content (brightness row shown / hidden) */
    GetWindowRect(g.hwnd, &wr); GetClientRect(g.hwnd, &cr);
    if (cr.bottom != S(y) || cr.right != S(W))
        SetWindowPos(g.hwnd, NULL, 0, 0, (wr.right - wr.left) - cr.right + S(W), (wr.bottom - wr.top) - cr.bottom + S(y), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g.hwnd, NULL, TRUE);
}

static void MakeControls(HWND h)
{
    struct { const wchar_t *cls, *text; DWORD style; int id; } c[] = {
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_INFO },
        { L"STATIC", L"Multiple displays:", SS_LEFT, IDC_L_MODE },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_MODE },
        { L"STATIC", L"Display:", SS_LEFT, IDC_L_MON },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_MON },
        { L"STATIC", L"Resolution:", SS_LEFT, IDC_L_RES },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_RES },
        { L"STATIC", L"Refresh rate:", SS_LEFT, IDC_L_HZ },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_HZ },
        { L"STATIC", L"Orientation:", SS_LEFT, IDC_L_ROT },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_ROT },
        { L"BUTTON", L"Make this my main display", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PRIMARY },
        { L"STATIC", L"Brightness:", SS_LEFT, IDC_L_BRI },
        { TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_BRIGHT },
        { L"STATIC", L"", SS_LEFT, IDC_BRIVAL },
        { L"STATIC", L"Scale (all displays):", SS_LEFT, IDC_L_DPI },
        { L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_DPI },
        { L"BUTTON", L"Restart shell now", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESTARTSHELL },
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_DPINOTE },
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_STATUS },
        { L"BUTTON", L"Save as default", BS_PUSHBUTTON | WS_TABSTOP, IDC_SAVE },
        { L"BUTTON", L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_APPLY },
        { L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE },
    };
    size_t i;
    for (i = 0; i < _countof(c); i++) {
        BOOL hidden = c[i].id == IDC_L_BRI || c[i].id == IDC_BRIGHT || c[i].id == IDC_BRIVAL;
        CreateWindowExW(0, c[i].cls, c[i].text, WS_CHILD | (hidden ? 0 : WS_VISIBLE) | c[i].style, 0, 0, 10, 10, h, (HMENU)(INT_PTR)c[i].id, g.hi, NULL);
    }
    if (!GetDlgItem(h, IDC_BRIGHT)) Log(L"display: brightness trackbar not created err=%lu", GetLastError());
    SendMessageW(GetDlgItem(h, IDC_BRIGHT), TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(GetDlgItem(h, IDC_BRIGHT), TBM_SETPAGESIZE, 0, 10);
    FillScaleList();
}

static void OnCommand(int id, int code)
{
    LPARAM v;
    Pend *p = &g.pend[g.sel];
    int k;
    switch (id) {
    case IDC_MODE:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_MODE)) >= 0) {
            g.modePend = (int)v;
            Log(L"display: mode selection %d (current %d)", g.modePend, g.modeCur);
            if (g.modePend != g.modeCur) { for (k = 0; k < g.st.n; k++) { g.pend[k].w = g.st.m[k].nw; g.pend[k].h = g.st.m[k].nh; g.pend[k].hz = g.st.m[k].hz; g.pend[k].rot = g.st.m[k].rot; g.pend[k].primary = g.st.m[k].primary; } FillMonitorControls(); SetStatus(L"Press Apply to change the mode. Resolution and orientation can be set afterwards."); }
            else SetStatus(L"");
            UpdateEnable();
        }
        break;
    case IDC_MON:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_MON)) >= 0) SelectMonitor((int)v, L"list");
        break;
    case IDC_RES:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_RES)) >= 0) {
            p->w = LOWORD(v); p->h = HIWORD(v);
            FillHz();
            Log(L"display: monitor %d resolution selection %lux%lu@%lu", g.sel + 1, p->w, p->h, p->hz);
            if (g.st.topo == DC_TOPO_CLONE) for (k = 0; k < g.st.n; k++) if (k != g.sel && !_wcsicmp(g.st.m[k].gdi, g.st.m[g.sel].gdi)) g.pend[k] = *p;
            UpdateEnable();
        }
        break;
    case IDC_HZ:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_HZ)) >= 0) { p->hz = (DWORD)v; Log(L"display: monitor %d refresh selection %lu", g.sel + 1, p->hz); UpdateEnable(); }
        break;
    case IDC_ROT:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_ROT)) >= 0) { p->rot = (int)v; Log(L"display: monitor %d orientation selection %d", g.sel + 1, p->rot); UpdateEnable(); }
        break;
    case IDC_PRIMARY:
        if (code == BN_CLICKED) {
            BOOL on = SendMessageW(Ctl(IDC_PRIMARY), BM_GETCHECK, 0, 0) == BST_CHECKED;
            for (k = 0; k < g.st.n; k++) g.pend[k].primary = on ? (k == g.sel) : g.st.m[k].primary;
            Log(L"display: monitor %d main display selection %d", g.sel + 1, on);
            UpdateEnable();
        }
        break;
    case IDC_DPI:
        if (code == CBN_SELCHANGE && (v = CbData(IDC_DPI)) > 0) { g.dpiPend = (DWORD)v; Log(L"display: scale selection %lu dpi", g.dpiPend); UpdateDpiNote(); UpdateEnable(); }
        break;
    case IDC_RESTARTSHELL: if (code == BN_CLICKED) RestartShell(); break;
    case IDC_SAVE: if (code == BN_CLICKED) SaveDefault(); break;
    case IDC_APPLY: case IDOK: if (code == BN_CLICKED || id == IDOK) Apply(); break;
    case IDC_CLOSE: case IDCANCEL: if (!g.busy && !g.confirming) DestroyWindow(g.hwnd); break;
    }
}

static LRESULT CALLBACK DisplayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT x;
        if (IntersectRect(&x, &ps.rcPaint, &g.map)) PaintMap(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        MapClick((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == Ctl(IDC_BRIGHT) && g.briOf[g.sel] >= 0) {
            int pos = (int)SendMessageW(Ctl(IDC_BRIGHT), TBM_GETPOS, 0, 0);
            wchar_t v[16];
            _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d%%", pos);
            SetWindowTextW(Ctl(IDC_BRIVAL), v);
            g.briPct[g.briOf[g.sel]] = pos;
            InterlockedExchange(&g.briWantTarget, g.briOf[g.sel]);
            InterlockedExchange(&g.briWantGen, g.briGenUI);
            InterlockedExchange(&g.briWant, pos);
            SetEvent(g.briEvent);
        }
        return 0;
    case WM_APP_BRI:
        TakeBrightness();
        MapBrightness();
        if (g.nbri <= 0) Log(L"display: brightness row hidden (no controllable brightness: no WMI panel, no DDC/CI monitor)");
        else {
            int k;
            for (k = 0; k < g.st.n; k++) Log(L"display: brightness for monitor %d: %s", k + 1, g.briOf[k] >= 0 ? Bri_KindName(g.bri[g.briOf[k]].kind) : L"none");
        }
        UpdateBrightnessRow();
        Log(L"display: brightness row %s for the selected monitor %d", g.briShown ? L"SHOWN" : L"hidden", g.sel + 1);
        return 0;
    case WM_DISPLAYCHANGE:
        if (!g.busy) SetTimer(h, TM_REQUERY, 500, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TM_REQUERY) {
            KillTimer(h, TM_REQUERY);
            if (!g.busy) {
                Requery(L"display change");
                if (!g.confirming) {   /* monitor handles changed: no brightness row until the new list arrives */
                    g.nbri = -1; MapBrightness(); UpdateBrightnessRow();
                    InterlockedExchange(&g.briReprobe, 1); SetEvent(g.briEvent);
                }
            }
        }
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g.s = HIWORD(wp) / 96.0;
        CreateFonts();
        EnumChildWindows(h, SetFontCb, (LPARAM)g.font);
        SendMessageW(Ctl(IDC_INFO), WM_SETFONT, (WPARAM)g.fontBold, TRUE);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        Log(L"display: window DPI changed to %u", (unsigned)HIWORD(wp));
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == Ctl(IDC_STATUS)) { SetTextColor((HDC)wp, RGB(0, 70, 140)); SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE)); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_CLOSE:
        if (g.busy || g.confirming) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int DisplayWindow(const wchar_t *uri, int argc, wchar_t **argv)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    HANDLE mutex;
    MSG msg;
    RECT rc;
    MONITORINFO mi;
    POINT pt;
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    int i, want = -1;
    wchar_t d[1400];

    mutex = CreateMutexW(NULL, TRUE, DISP_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(DISP_CLASS, NULL);
        Log(L"display: window already open (hwnd %p) - brought to front", (void *)other);
        if (other) { if (IsIconic(other)) ShowWindow(other, SW_RESTORE); ForceForeground(other); }
        CloseHandle(mutex);
        return 0;
    }
    ZeroMemory(&g, sizeof(g));
    g.hi = GetModuleHandleW(NULL);
    g.briWant = -1; g.briWantTarget = -1; g.nbri = -1;
    for (i = 0; i < DC_MAXMON; i++) g.briOf[i] = -1;
    for (i = 0; i < argc; i++) if (!_wcsicmp(argv[i], L"/select") && i + 1 < argc) want = _wtoi(argv[i + 1]) - 1;
    if (uri && !_wcsnicmp(uri, L"ms-settings:screenrotation", 26)) g.selectRotation = TRUE;

    /* ICC_BAR_CLASSES only: comctl32 v5 (no manifest) rejects the v6-only ICC_STANDARD_CLASSES (VM test 2026-11-23) */
    icc.dwSize = sizeof(icc); icc.dwICC = ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&icc)) Log(L"display: InitCommonControlsEx failed err=%lu (brightness slider may be missing)", GetLastError());
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = DisplayProc; wc.hInstance = g.hi; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = DISP_CLASS;
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) { Log(L"display: RegisterClass failed err=%lu", GetLastError()); return 1; }
    wc.lpfnWndProc = ConfirmProc; wc.lpszClassName = CONF_CLASS; wc.hIcon = NULL;
    if (!RegisterClassExW(&wc)) { Log(L"display: RegisterClass (confirm) failed err=%lu", GetLastError()); return 1; }

    {   /* scale in use (live, scale.c); the registry value only as a fallback */
        int cur = 0, max = 0;
        if (Scale_Query(&cur, &max)) { g.dpiCur = (DWORD)(cur * 96 / 100); s_scaleMax = max; }
        else g.dpiCur = ReadLogPixels();
        g.dpiPend = g.dpiCur;
        g.dpiSystem = SystemDpi();
        Log(L"display: scale: current %lu%% (displays accept up to %d%%), system dpi=%lu, LogPixels=%lu", g.dpiCur * 100 / 96, s_scaleMax, g.dpiSystem, ReadLogPixels());
    }

    /* main display: initial size at its DPI */
    pt.x = 0; pt.y = 0;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, DISP_CLASS, L"Display", st, mi.rcWork.left + 40, mi.rcWork.top + 40, 600, 700, NULL, NULL, g.hi, NULL);
    if (!g.hwnd) { Log(L"display: CreateWindow failed err=%lu", GetLastError()); return 1; }
    g.s = WindowDpi(g.hwnd) / 96.0;
    CreateFonts();
    MakeControls(g.hwnd);
    EnumChildWindows(g.hwnd, SetFontCb, (LPARAM)g.font);
    SendMessageW(Ctl(IDC_INFO), WM_SETFONT, (WPARAM)g.fontBold, TRUE);
    Layout();
    Requery(L"open");
    if (want >= 0 && want < g.st.n) SelectMonitor(want, L"/select");
    else for (i = 0; i < g.st.n; i++) if (g.st.m[i].primary && g.st.m[i].active) { SelectMonitor(i, L"main display"); break; }
    GetWindowRect(g.hwnd, &rc);
    SetWindowPos(g.hwnd, NULL, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2,
                 mi.rcWork.top + max(0, ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g.hwnd, SW_SHOW);
    ForceForeground(g.hwnd);
    if (g.selectRotation) SetFocus(Ctl(IDC_ROT));
    GetWindowRect(g.hwnd, &rc);
    Dc_Describe(&g.st, d, _countof(d));
    Log(L"display: window open (uri=%s) dpi-scale=%.2f rect=%ld,%ld,%ldx%ld: %s", uri ? uri : L"", g.s, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, d);

    InitializeCriticalSection(&s_briCs);
    g.briEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g.briThread = g.briEvent ? CreateThread(NULL, 0, BriWorker, NULL, 0, NULL) : NULL;
    if (!g.briThread) Log(L"display: brightness worker not started err=%lu (row stays hidden)", GetLastError());

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g.confirm && IsDialogMessageW(g.confirm, &msg)) continue;
        if (IsDialogMessageW(g.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.briThread) {
        InterlockedExchange(&g.briQuit, 1);
        SetEvent(g.briEvent);
        if (WaitForSingleObject(g.briThread, 5000) != WAIT_OBJECT_0) Log(L"display: brightness worker did not stop in 5 s");
        CloseHandle(g.briThread);
    }
    if (g.briEvent) CloseHandle(g.briEvent);
    if (g.font) DeleteObject(g.font);
    if (g.fontBold) DeleteObject(g.fontBold);
    Log(L"display: window closed");
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
