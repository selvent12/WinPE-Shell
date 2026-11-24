/*
 * brightness.c - see brightness.h. Own code; WMI through COM (wbemidl.h), DDC/CI through dxva2.dll loaded at run time.
 */
#define COBJMACROS
#include "brightness.h"
#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct { HANDLE h; WCHAR desc[128]; } PhysMon;   /* = PHYSICAL_MONITOR (physicalmonitorenumerationapi.h) */
typedef BOOL (WINAPI *PfnNumPhys)(HMONITOR, LPDWORD);
typedef BOOL (WINAPI *PfnGetPhys)(HMONITOR, DWORD, PhysMon *);
typedef BOOL (WINAPI *PfnDestroyPhys)(HANDLE);
typedef BOOL (WINAPI *PfnGetBri)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *PfnSetBri)(HANDLE, DWORD);

static struct {
    HMODULE dll;
    PfnNumPhys num; PfnGetPhys get; PfnDestroyPhys destroy; PfnGetBri getBri; PfnSetBri setBri;
} g_dx;

const wchar_t *Bri_KindName(int kind) { return kind == BRI_WMI ? L"WMI" : (kind == BRI_DDC ? L"DDC/CI" : L"?"); }

static BOOL LoadDxva2(void)
{
    wchar_t path[MAX_PATH];
    if (g_dx.dll) return TRUE;
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, MAX_PATH, L"\\dxva2.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { Log(L"brightness: %s not in the image - no DDC/CI", path); return FALSE; }
    g_dx.dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dx.dll) { Log(L"brightness: LoadLibrary dxva2.dll failed err=%lu", GetLastError()); return FALSE; }
    g_dx.num = (PfnNumPhys)(void *)GetProcAddress(g_dx.dll, "GetNumberOfPhysicalMonitorsFromHMONITOR");
    g_dx.get = (PfnGetPhys)(void *)GetProcAddress(g_dx.dll, "GetPhysicalMonitorsFromHMONITOR");
    g_dx.destroy = (PfnDestroyPhys)(void *)GetProcAddress(g_dx.dll, "DestroyPhysicalMonitor");
    g_dx.getBri = (PfnGetBri)(void *)GetProcAddress(g_dx.dll, "GetMonitorBrightness");
    g_dx.setBri = (PfnSetBri)(void *)GetProcAddress(g_dx.dll, "SetMonitorBrightness");
    if (!g_dx.num || !g_dx.get || !g_dx.destroy || !g_dx.getBri || !g_dx.setBri) {
        Log(L"brightness: dxva2.dll lacks the monitor configuration functions");
        FreeLibrary(g_dx.dll); ZeroMemory(&g_dx, sizeof(g_dx));
        return FALSE;
    }
    return TRUE;
}

/* ---------------- WMI ---------------- */

static HRESULT WmiConnect(IWbemServices **svc)
{
    IWbemLocator *loc = NULL;
    BSTR ns;
    HRESULT hr;
    *svc = NULL;
    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr)) return hr;
    ns = SysAllocString(L"ROOT\\WMI");
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, svc);
    SysFreeString(ns);
    IWbemLocator_Release(loc);
    if (FAILED(hr)) return hr;
    hr = CoSetProxyBlanket((IUnknown *)*svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) { IWbemServices_Release(*svc); *svc = NULL; }
    return hr;
}

static HRESULT WmiQuery(IWbemServices *svc, const wchar_t *wql, IEnumWbemClassObject **en)
{
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(wql);
    HRESULT hr = IWbemServices_ExecQuery(svc, lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, en);
    SysFreeString(lang); SysFreeString(q);
    return hr;
}

static void GetStr(IWbemClassObject *o, const wchar_t *name, wchar_t *out, size_t cch)
{
    VARIANT v;
    out[0] = 0;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(o, name, 0, &v, NULL, NULL)) && v.vt == VT_BSTR && v.bstrVal) wcsncpy_s(out, cch, v.bstrVal, _TRUNCATE);
    VariantClear(&v);
}

static int GetInt(IWbemClassObject *o, const wchar_t *name, int def)
{
    VARIANT v;
    int r = def;
    VariantInit(&v);
    if (SUCCEEDED(IWbemClassObject_Get(o, name, 0, &v, NULL, NULL))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_I4) r = v.lVal;
        else if (v.vt == VT_BOOL) r = v.boolVal ? 1 : 0;
        else if (v.vt == VT_I2) r = v.iVal;
    }
    VariantClear(&v);
    return r;
}

static int WmiProbe(BriTarget *t, int max)
{
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *o = NULL;
    ULONG got = 0;
    int n = 0;
    HRESULT hr = WmiConnect(&svc);
    if (FAILED(hr)) { Log(L"brightness: WMI connect ROOT\\WMI failed hr=0x%08lX", (unsigned long)hr); return 0; }
    hr = WmiQuery(svc, L"SELECT * FROM WmiMonitorBrightness", &en);
    if (FAILED(hr)) { Log(L"brightness: WMI query WmiMonitorBrightness failed hr=0x%08lX", (unsigned long)hr); IWbemServices_Release(svc); return 0; }
    for (;;) {
        VARIANT v;
        BriTarget *b;
        hr = IEnumWbemClassObject_Next(en, 8000, 1, &o, &got);
        if (hr != WBEM_S_NO_ERROR || !got) {
            if (FAILED(hr)) Log(L"brightness: WMI WmiMonitorBrightness enumeration hr=0x%08lX (no laptop panel / provider missing)", (unsigned long)hr);
            break;
        }
        if (n < max && GetInt(o, L"Active", 1)) {
            b = &t[n];
            ZeroMemory(b, sizeof(*b));
            b->kind = BRI_WMI;
            GetStr(o, L"InstanceName", b->instance, _countof(b->instance));
            wcscpy_s(b->desc, _countof(b->desc), L"Built-in display");
            VariantInit(&v);
            if (SUCCEEDED(IWbemClassObject_Get(o, L"Level", 0, &v, NULL, NULL)) && (v.vt & VT_ARRAY) && v.parray) {
                LONG lo = 0, hi = -1, i;
                SafeArrayGetLBound(v.parray, 1, &lo); SafeArrayGetUBound(v.parray, 1, &hi);
                for (i = lo; i <= hi && b->nlevels < 101; i++) { BYTE lv = 0; if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &lv))) b->levels[b->nlevels++] = lv; }
            }
            VariantClear(&v);
            Log(L"brightness: WMI panel '%s' current=%d levels=%d", b->instance, GetInt(o, L"CurrentBrightness", -1), b->nlevels);
            n++;
        }
        IWbemClassObject_Release(o); o = NULL;
    }
    IEnumWbemClassObject_Release(en);
    IWbemServices_Release(svc);
    return n;
}

static int WmiGet(BriTarget *t)
{
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *o = NULL;
    ULONG got = 0;
    int r = -1;
    wchar_t inst[256];
    if (FAILED(WmiConnect(&svc))) return -1;
    if (SUCCEEDED(WmiQuery(svc, L"SELECT * FROM WmiMonitorBrightness", &en))) {
        while (IEnumWbemClassObject_Next(en, 8000, 1, &o, &got) == WBEM_S_NO_ERROR && got) {
            GetStr(o, L"InstanceName", inst, _countof(inst));
            if (r < 0 && !_wcsicmp(inst, t->instance)) r = GetInt(o, L"CurrentBrightness", -1);
            IWbemClassObject_Release(o); o = NULL;
        }
        IEnumWbemClassObject_Release(en);
    }
    IWbemServices_Release(svc);
    return r;
}

static BOOL WmiSet(BriTarget *t, int pct)
{
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *o = NULL, *cls = NULL, *sig = NULL, *in = NULL;
    ULONG got = 0;
    wchar_t inst[256], path[512];
    BSTR bcls = NULL, bpath = NULL, bmeth = NULL;
    VARIANT v;
    HRESULT hr;
    BOOL ok = FALSE;
    path[0] = 0;
    if (FAILED(WmiConnect(&svc))) return FALSE;
    if (SUCCEEDED(WmiQuery(svc, L"SELECT * FROM WmiMonitorBrightnessMethods", &en))) {
        while (IEnumWbemClassObject_Next(en, 8000, 1, &o, &got) == WBEM_S_NO_ERROR && got) {
            GetStr(o, L"InstanceName", inst, _countof(inst));
            if (!path[0] && !_wcsicmp(inst, t->instance)) GetStr(o, L"__PATH", path, _countof(path));
            IWbemClassObject_Release(o); o = NULL;
        }
        IEnumWbemClassObject_Release(en);
    }
    if (!path[0]) { Log(L"brightness: WMI no WmiMonitorBrightnessMethods for '%s'", t->instance); IWbemServices_Release(svc); return FALSE; }
    bcls = SysAllocString(L"WmiMonitorBrightnessMethods");
    bmeth = SysAllocString(L"WmiSetBrightness");
    bpath = SysAllocString(path);
    hr = IWbemServices_GetObject(svc, bcls, 0, NULL, &cls, NULL);
    if (SUCCEEDED(hr)) hr = IWbemClassObject_GetMethod(cls, bmeth, 0, &sig, NULL);
    if (SUCCEEDED(hr)) hr = IWbemClassObject_SpawnInstance(sig, 0, &in);
    if (SUCCEEDED(hr)) { VariantInit(&v); v.vt = VT_I4; v.lVal = 0; hr = IWbemClassObject_Put(in, L"Timeout", 0, &v, 0); }
    if (SUCCEEDED(hr)) { VariantInit(&v); v.vt = VT_UI1; v.bVal = (BYTE)pct; hr = IWbemClassObject_Put(in, L"Brightness", 0, &v, 0); }
    if (SUCCEEDED(hr)) hr = IWbemServices_ExecMethod(svc, bpath, bmeth, 0, NULL, in, NULL, NULL);
    ok = SUCCEEDED(hr);
    if (!ok) Log(L"brightness: WMI WmiSetBrightness(%d) failed hr=0x%08lX", pct, (unsigned long)hr);
    if (in) IWbemClassObject_Release(in);
    if (sig) IWbemClassObject_Release(sig);
    if (cls) IWbemClassObject_Release(cls);
    SysFreeString(bcls); SysFreeString(bmeth); SysFreeString(bpath);
    IWbemServices_Release(svc);
    return ok;
}

/* ---------------- DDC/CI ---------------- */

typedef struct { BriTarget *t; int max, n; } DdcCtx;

static BOOL CALLBACK DdcMonitorCb(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp)
{
    DdcCtx *c = (DdcCtx *)lp;
    DWORD cnt = 0, i;
    PhysMon pm[4];
    MONITORINFOEXW mi;
    (void)dc; (void)rc;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) mi.szDevice[0] = 0;
    if (!g_dx.num(mon, &cnt) || !cnt) { Log(L"brightness: DDC %s: no physical monitor (err=%lu)", mi.szDevice, GetLastError()); return TRUE; }
    if (cnt > 4) cnt = 4;
    if (!g_dx.get(mon, cnt, pm)) { Log(L"brightness: DDC %s: GetPhysicalMonitors failed err=%lu", mi.szDevice, GetLastError()); return TRUE; }
    for (i = 0; i < cnt; i++) {
        DWORD mn = 0, cur = 0, mx = 0;
        if (c->n < c->max && g_dx.getBri(pm[i].h, &mn, &cur, &mx) && mx > mn) {
            BriTarget *b = &c->t[c->n++];
            ZeroMemory(b, sizeof(*b));
            b->kind = BRI_DDC; b->hmon = mon; b->phys = pm[i].h; b->minv = mn; b->maxv = mx;
            wcsncpy_s(b->gdi, _countof(b->gdi), mi.szDevice, _TRUNCATE);
            wcsncpy_s(b->desc, _countof(b->desc), pm[i].desc, _TRUNCATE);
            Log(L"brightness: DDC %s '%s' min=%lu cur=%lu max=%lu", mi.szDevice, pm[i].desc, mn, cur, mx);
        } else {
            Log(L"brightness: DDC %s '%s': no brightness control (err=%lu)", mi.szDevice, pm[i].desc, GetLastError());
            g_dx.destroy(pm[i].h);
        }
    }
    return TRUE;
}

/* ---------------- public ---------------- */

int Bri_Probe(BriTarget *t, int max)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int n = 0;
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) Log(L"brightness: CoInitializeEx hr=0x%08lX", (unsigned long)hr);
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) Log(L"brightness: CoInitializeSecurity hr=0x%08lX", (unsigned long)hr);
    n = WmiProbe(t, max);
    if (LoadDxva2()) {
        DdcCtx c;
        c.t = t; c.max = max; c.n = n;
        EnumDisplayMonitors(NULL, NULL, DdcMonitorCb, (LPARAM)&c);
        n = c.n;
    }
    Log(L"brightness: %d controllable target(s)", n);
    return n;
}

int Bri_GetPercent(BriTarget *t)
{
    if (t->kind == BRI_WMI) return WmiGet(t);
    if (t->kind == BRI_DDC && g_dx.getBri) {
        DWORD mn = 0, cur = 0, mx = 0;
        if (!g_dx.getBri(t->phys, &mn, &cur, &mx) || mx <= mn) return -1;
        return (int)(((cur - mn) * 100 + (mx - mn) / 2) / (mx - mn));
    }
    return -1;
}

BOOL Bri_SetPercent(BriTarget *t, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (t->kind == BRI_WMI) {
        if (t->nlevels > 0) {   /* nearest supported level */
            int i, best = t->levels[0];
            for (i = 1; i < t->nlevels; i++) if (abs((int)t->levels[i] - pct) < abs(best - pct)) best = t->levels[i];
            pct = best;
        }
        return WmiSet(t, pct);
    }
    if (t->kind == BRI_DDC && g_dx.setBri) {
        DWORD raw = t->minv + (DWORD)((t->maxv - t->minv) * (DWORD)pct + 50) / 100;
        if (!g_dx.setBri(t->phys, raw)) { Log(L"brightness: DDC SetMonitorBrightness(%lu) failed err=%lu", raw, GetLastError()); return FALSE; }
        return TRUE;
    }
    return FALSE;
}

void Bri_Free(BriTarget *t, int n)
{
    int i;
    for (i = 0; i < n; i++) if (t[i].kind == BRI_DDC && t[i].phys && g_dx.destroy) { g_dx.destroy(t[i].phys); t[i].phys = NULL; }
}
