/*
 * dispcfg.c - display configuration helpers (see dispcfg.h). Own code, Win32 only.
 */
#include "dispcfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>

const wchar_t *Dc_TopoName(int topo)
{
    switch (topo) {
    case DC_TOPO_SINGLE: return L"single";
    case DC_TOPO_EXTEND: return L"extend";
    case DC_TOPO_CLONE:  return L"duplicate";
    case DC_TOPO_ONLY:   return L"only";
    }
    return L"none";
}

/* QueryDisplayConfig with retry (the configuration can change between the size query and the query) */
static LONG QueryCcd(UINT32 flags, UINT32 *np, DISPLAYCONFIG_PATH_INFO **p, UINT32 *nm, DISPLAYCONFIG_MODE_INFO **m)
{
    LONG rc = ERROR_GEN_FAILURE;
    int tries;
    *p = NULL; *m = NULL; *np = *nm = 0;
    for (tries = 0; tries < 4; tries++) {
        UINT32 a = 0, b = 0;
        rc = GetDisplayConfigBufferSizes(flags, &a, &b);
        if (rc != ERROR_SUCCESS) return rc;
        *p = (DISPLAYCONFIG_PATH_INFO *)calloc(a ? a : 1, sizeof(**p));
        *m = (DISPLAYCONFIG_MODE_INFO *)calloc(b ? b : 1, sizeof(**m));
        if (!*p || !*m) { free(*p); free(*m); *p = NULL; *m = NULL; return ERROR_OUTOFMEMORY; }
        rc = QueryDisplayConfig(flags, &a, *p, &b, *m, NULL);
        if (rc == ERROR_SUCCESS) { *np = a; *nm = b; return rc; }
        free(*p); free(*m); *p = NULL; *m = NULL;
        if (rc != ERROR_INSUFFICIENT_BUFFER) return rc;
    }
    return rc;
}

static BOOL SameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

static void TargetInfo(DcMonitor *mo)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
    DISPLAYCONFIG_TARGET_PREFERRED_MODE pm;
    ZeroMemory(&tn, sizeof(tn));
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = mo->adapter;
    tn.header.id = mo->targetId;
    if (DisplayConfigGetDeviceInfo(&tn.header) == ERROR_SUCCESS) {
        if (tn.monitorFriendlyDeviceName[0]) wcsncpy_s(mo->name, _countof(mo->name), tn.monitorFriendlyDeviceName, _TRUNCATE);
        switch (tn.outputTechnology) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
            mo->internal = TRUE; break;
        default: break;
        }
    }
    ZeroMemory(&pm, sizeof(pm));
    pm.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
    pm.header.size = sizeof(pm);
    pm.header.adapterId = mo->adapter;
    pm.header.id = mo->targetId;
    if (DisplayConfigGetDeviceInfo(&pm.header) == ERROR_SUCCESS) { mo->prefW = pm.width; mo->prefH = pm.height; }
    if (!mo->name[0]) wcscpy_s(mo->name, _countof(mo->name), L"Generic monitor");
}

static void SourceName(LUID adapter, UINT32 id, wchar_t *gdi, size_t cch)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sn;
    ZeroMemory(&sn, sizeof(sn));
    sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sn.header.size = sizeof(sn);
    sn.header.adapterId = adapter;
    sn.header.id = id;
    gdi[0] = 0;
    if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) wcsncpy_s(gdi, cch, sn.viewGdiDeviceName, _TRUNCATE);
}

static int CmpMode(const void *a, const void *b)
{
    const DcMode *x = (const DcMode *)a, *y = (const DcMode *)b;
    ULONGLONG ax = (ULONGLONG)x->w * x->h, ay = (ULONGLONG)y->w * y->h;
    if (ax != ay) return ax > ay ? -1 : 1;
    if (x->w != y->w) return x->w > y->w ? -1 : 1;
    if (x->hz != y->hz) return x->hz > y->hz ? -1 : 1;
    return 0;
}

/* modes of an active GDI source, normalised to the unrotated orientation */
static void ReadModes(DcMonitor *mo)
{
    DEVMODEW dm;
    DcMode raw[DC_MAXMODES];
    int nraw = 0, i, j, has32 = 0, swap = 0, curRot = 0, curUnrot = 0;
    DWORD k;
    for (k = 0; nraw < DC_MAXMODES; k++) {
        ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(mo->gdi, k, &dm)) break;
        if (dm.dmBitsPerPel == 32) has32 = 1;
        if (k > 4000) break;
    }
    for (k = 0; nraw < DC_MAXMODES; k++) {
        ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(mo->gdi, k, &dm)) break;
        if (k > 4000) break;
        if (has32 && dm.dmBitsPerPel != 32) continue;
        if (!dm.dmPelsWidth || !dm.dmPelsHeight) continue;
        raw[nraw].w = dm.dmPelsWidth; raw[nraw].h = dm.dmPelsHeight; raw[nraw].hz = dm.dmDisplayFrequency;
        for (j = 0; j < nraw; j++) if (raw[j].w == raw[nraw].w && raw[j].h == raw[nraw].h && raw[j].hz == raw[nraw].hz) break;
        if (j == nraw) nraw++;
    }
    /* rotated by 90/270: if the list contains the rotated current size but not the unrotated one, it is rotated */
    if (mo->rot == 90 || mo->rot == 270) {
        for (i = 0; i < nraw; i++) {
            if (raw[i].w == mo->w && raw[i].h == mo->h) curRot = 1;
            if (raw[i].w == mo->nw && raw[i].h == mo->nh) curUnrot = 1;
        }
        swap = curRot && !curUnrot;
    }
    mo->nmodes = 0;
    for (i = 0; i < nraw; i++) {
        DcMode md = raw[i];
        if (swap) { DWORD t = md.w; md.w = md.h; md.h = t; }
        mo->modes[mo->nmodes++] = md;
    }
    qsort(mo->modes, (size_t)mo->nmodes, sizeof(DcMode), CmpMode);
}

BOOL Dc_Query(DcState *s)
{
    UINT32 np, nm, i;
    DISPLAYCONFIG_PATH_INFO *p;
    DISPLAYCONFIG_MODE_INFO *m;
    LONG rc;
    int k, j;
    ZeroMemory(s, sizeof(*s));
    rc = QueryCcd(QDC_ALL_PATHS, &np, &p, &nm, &m);
    if (rc != ERROR_SUCCESS) { Log(L"display: QueryDisplayConfig(ALL_PATHS) failed rc=%ld", rc); return FALSE; }

    for (i = 0; i < np; i++) {
        DISPLAYCONFIG_PATH_INFO *pi = &p[i];
        DcMonitor *mo = NULL;
        if (!pi->targetInfo.targetAvailable) continue;
        for (k = 0; k < s->n; k++)
            if (SameLuid(s->m[k].adapter, pi->targetInfo.adapterId) && s->m[k].targetId == pi->targetInfo.id) { mo = &s->m[k]; break; }
        if (!mo) {
            if (s->n >= DC_MAXMON) continue;
            mo = &s->m[s->n++];
            mo->adapter = pi->targetInfo.adapterId;
            mo->targetId = pi->targetInfo.id;
            TargetInfo(mo);
        }
        if ((pi->flags & DISPLAYCONFIG_PATH_ACTIVE) && !mo->active) {
            UINT32 si = pi->sourceInfo.modeInfoIdx;
            mo->active = TRUE;
            mo->sourceId = pi->sourceInfo.id;
            SourceName(pi->sourceInfo.adapterId, pi->sourceInfo.id, mo->gdi, _countof(mo->gdi));
            if (si != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && si < nm && m[si].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                mo->x = m[si].sourceMode.position.x; mo->y = m[si].sourceMode.position.y;
                mo->w = m[si].sourceMode.width; mo->h = m[si].sourceMode.height;
            }
            switch (pi->targetInfo.rotation) {
            case DISPLAYCONFIG_ROTATION_ROTATE90:  mo->rot = 90; break;
            case DISPLAYCONFIG_ROTATION_ROTATE180: mo->rot = 180; break;
            case DISPLAYCONFIG_ROTATION_ROTATE270: mo->rot = 270; break;
            default: mo->rot = 0; break;
            }
            if (pi->targetInfo.refreshRate.Denominator)
                mo->hz = (DWORD)((pi->targetInfo.refreshRate.Numerator + pi->targetInfo.refreshRate.Denominator / 2) / pi->targetInfo.refreshRate.Denominator);
        }
    }
    free(p); free(m);

    /* GDI view of the active sources: current mode, orientation, primary, adapter name, mode list */
    for (k = 0; k < s->n; k++) {
        DcMonitor *mo = &s->m[k];
        DISPLAY_DEVICEW dd;
        DEVMODEW dm;
        DWORD d;
        if (!mo->active || !mo->gdi[0]) continue;
        ZeroMemory(&dm, sizeof(dm)); dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mo->gdi, ENUM_CURRENT_SETTINGS, &dm)) {
            mo->w = dm.dmPelsWidth; mo->h = dm.dmPelsHeight;
            mo->x = dm.dmPosition.x; mo->y = dm.dmPosition.y;
            if (dm.dmDisplayFrequency > 1) mo->hz = dm.dmDisplayFrequency;
            if (dm.dmFields & DM_DISPLAYORIENTATION) mo->rot = (int)dm.dmDisplayOrientation * 90;
        }
        mo->nw = (mo->rot == 90 || mo->rot == 270) ? mo->h : mo->w;
        mo->nh = (mo->rot == 90 || mo->rot == 270) ? mo->w : mo->h;
        for (d = 0; d < 32; d++) {
            ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
            if (!EnumDisplayDevicesW(NULL, d, &dd, 0)) break;
            if (!_wcsicmp(dd.DeviceName, mo->gdi)) {
                wcsncpy_s(mo->adapterName, _countof(mo->adapterName), dd.DeviceString, _TRUNCATE);
                mo->primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                break;
            }
        }
        if (mo->x == 0 && mo->y == 0) mo->primary = TRUE;
        ReadModes(mo);
    }
    /* adapter name for inactive monitors: from an active one on the same adapter */
    for (k = 0; k < s->n; k++) {
        if (s->m[k].adapterName[0]) continue;
        for (j = 0; j < s->n; j++)
            if (s->m[j].adapterName[0] && SameLuid(s->m[j].adapter, s->m[k].adapter)) { wcscpy_s(s->m[k].adapterName, _countof(s->m[k].adapterName), s->m[j].adapterName); break; }
    }
    /* topology */
    for (k = 0; k < s->n; k++) {
        if (!s->m[k].active) continue;
        s->nactive++;
        for (j = 0; j < k; j++)
            if (s->m[j].active && SameLuid(s->m[j].adapter, s->m[k].adapter) && s->m[j].sourceId == s->m[k].sourceId) break;
        if (j == k) s->nsources++;
    }
    if (s->n == 0) s->topo = DC_TOPO_NONE;
    else if (s->n == 1) s->topo = DC_TOPO_SINGLE;
    else if (s->nactive >= 2 && s->nsources == 1) s->topo = DC_TOPO_CLONE;
    else if (s->nactive >= 2) s->topo = DC_TOPO_EXTEND;
    else {
        s->topo = DC_TOPO_ONLY;
        for (k = 0; k < s->n; k++) if (s->m[k].active) s->only = k;
    }
    return TRUE;
}

void Dc_Describe(const DcState *s, wchar_t *buf, size_t cch)
{
    int k;
    size_t len;
    _snwprintf_s(buf, cch, _TRUNCATE, L"monitors=%d active=%d sources=%d topology=%s", s->n, s->nactive, s->nsources, Dc_TopoName(s->topo));
    for (k = 0; k < s->n; k++) {
        const DcMonitor *mo = &s->m[k];
        len = wcslen(buf);
        if (mo->active)
            _snwprintf_s(buf + len, cch - len, _TRUNCATE, L" | %d:'%s' %s %lux%lu@%lu rot=%d pos=%d,%d%s%s modes=%d",
                         k + 1, mo->name, mo->gdi, mo->nw, mo->nh, mo->hz, mo->rot, mo->x, mo->y,
                         mo->primary ? L" primary" : L"", mo->internal ? L" internal" : L"", mo->nmodes);
        else
            _snwprintf_s(buf + len, cch - len, _TRUNCATE, L" | %d:'%s' off%s", k + 1, mo->name, mo->internal ? L" internal" : L"");
    }
}

/* supplied configuration: the listed monitors become active (mode indexes invalid: the system picks modes and positions).
   clone=TRUE: all on one source. */
static LONG SetSupplied(const DcState *s, const int *idx, int cnt, BOOL clone)
{
    UINT32 np, nm, i;
    DISPLAYCONFIG_PATH_INFO *p, sel[DC_MAXMON];
    DISPLAYCONFIG_MODE_INFO *m;
    UINT32 usedSrc[DC_MAXMON];
    LUID usedAd[DC_MAXMON];
    int nsel = 0, nused = 0, c, u;
    LONG rc = QueryCcd(QDC_ALL_PATHS, &np, &p, &nm, &m);
    if (rc != ERROR_SUCCESS) return rc;
    for (c = 0; c < cnt; c++) {
        const DcMonitor *mo = &s->m[idx[c]];
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_PATH_INFO *pi = &p[i];
            if (!pi->targetInfo.targetAvailable || pi->targetInfo.id != mo->targetId || !SameLuid(pi->targetInfo.adapterId, mo->adapter)) continue;
            if (clone && nsel > 0) {
                if (pi->sourceInfo.id != sel[0].sourceInfo.id || !SameLuid(pi->sourceInfo.adapterId, sel[0].sourceInfo.adapterId)) continue;
            } else {
                for (u = 0; u < nused; u++) if (usedSrc[u] == pi->sourceInfo.id && SameLuid(usedAd[u], pi->sourceInfo.adapterId)) break;
                if (u < nused) continue;
            }
            break;
        }
        if (i == np) { Log(L"display: no usable path for monitor %d (clone=%d)", idx[c] + 1, clone); free(p); free(m); return ERROR_NOT_FOUND; }
        sel[nsel] = p[i];
        sel[nsel].flags = DISPLAYCONFIG_PATH_ACTIVE;
        sel[nsel].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        sel[nsel].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        nsel++;
        if (nused < DC_MAXMON) { usedSrc[nused] = p[i].sourceInfo.id; usedAd[nused] = p[i].sourceInfo.adapterId; nused++; }
    }
    free(p); free(m);
    rc = SetDisplayConfig((UINT32)nsel, sel, 0, NULL, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    Log(L"display: SetDisplayConfig(supplied, %d paths, clone=%d) rc=%ld", nsel, clone, rc);
    return rc;
}

static BOOL TopoReached(int topo, int only)
{
    DcState now;
    if (!Dc_Query(&now)) return FALSE;
    if (topo == DC_TOPO_ONLY) return now.topo == DC_TOPO_ONLY && now.only == only;
    return now.topo == topo;
}

LONG Dc_SetTopology(const DcState *s, int topo, int only)
{
    int idx[DC_MAXMON], k, cnt = 0;
    LONG rc;
    if (topo == DC_TOPO_EXTEND || topo == DC_TOPO_CLONE) {
        UINT32 f = SDC_APPLY | (topo == DC_TOPO_EXTEND ? SDC_TOPOLOGY_EXTEND : SDC_TOPOLOGY_CLONE);
        rc = SetDisplayConfig(0, NULL, 0, NULL, f);
        Log(L"display: SetDisplayConfig(TOPOLOGY_%s) rc=%ld", topo == DC_TOPO_EXTEND ? L"EXTEND" : L"CLONE", rc);
        if (rc == ERROR_SUCCESS && TopoReached(topo, 0)) return rc;
        /* no database entry for this topology (fresh PE) or a driver refusal: explicit paths */
        for (k = 0; k < s->n; k++) idx[cnt++] = k;
        rc = SetSupplied(s, idx, cnt, topo == DC_TOPO_CLONE);
        if (rc == ERROR_SUCCESS && !TopoReached(topo, 0)) { Log(L"display: topology %s not reached after supplied config", Dc_TopoName(topo)); rc = ERROR_GEN_FAILURE; }
        return rc;
    }
    if (topo == DC_TOPO_ONLY && only >= 0 && only < s->n) {
        idx[0] = only;
        rc = SetSupplied(s, idx, 1, FALSE);
        if (rc == ERROR_SUCCESS && !TopoReached(topo, only)) { Log(L"display: 'only %d' not reached", only + 1); rc = ERROR_GEN_FAILURE; }
        return rc;
    }
    return ERROR_INVALID_PARAMETER;
}

static const wchar_t *DispChangeName(LONG r)
{
    switch (r) {
    case DISP_CHANGE_SUCCESSFUL:  return L"ok";
    case DISP_CHANGE_RESTART:     return L"restart required";
    case DISP_CHANGE_FAILED:      return L"the driver failed the mode";
    case DISP_CHANGE_BADMODE:     return L"mode not supported";
    case DISP_CHANGE_NOTUPDATED:  return L"registry not updated";
    case DISP_CHANGE_BADFLAGS:    return L"bad flags";
    case DISP_CHANGE_BADPARAM:    return L"bad parameter";
    case DISP_CHANGE_BADDUALVIEW: return L"DualView error";
    }
    return L"unknown error";
}

static void FillDevmode(DEVMODEW *dm, const DcSet *e)
{
    ZeroMemory(dm, sizeof(*dm)); dm->dmSize = sizeof(*dm);
    if (!EnumDisplaySettingsW(e->gdi, ENUM_CURRENT_SETTINGS, dm)) { ZeroMemory(dm, sizeof(*dm)); dm->dmSize = sizeof(*dm); EnumDisplaySettingsW(e->gdi, ENUM_REGISTRY_SETTINGS, dm); }
    dm->dmSize = sizeof(*dm); dm->dmDriverExtra = 0;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_DISPLAYORIENTATION;
    if (e->rot == 90 || e->rot == 270) { dm->dmPelsWidth = e->h; dm->dmPelsHeight = e->w; }
    else { dm->dmPelsWidth = e->w; dm->dmPelsHeight = e->h; }
    dm->dmDisplayOrientation = (DWORD)(e->rot / 90);
    dm->dmPosition.x = e->x; dm->dmPosition.y = e->y;
    if (e->hz) { dm->dmDisplayFrequency = e->hz; dm->dmFields |= DM_DISPLAYFREQUENCY; }
}

LONG Dc_ApplyGdi(const DcSet *set, int n, wchar_t *err, size_t cch)
{
    DEVMODEW dm;
    LONG r;
    int i;
    if (err && cch) err[0] = 0;
    for (i = 0; i < n; i++) {   /* validate everything first: nothing changes if one mode is refused */
        FillDevmode(&dm, &set[i]);
        r = ChangeDisplaySettingsExW(set[i].gdi, &dm, NULL, CDS_TEST, NULL);
        Log(L"display: test %s %lux%lu@%lu rot=%d pos=%d,%d primary=%d -> %ld (%s)", set[i].gdi, set[i].w, set[i].h, set[i].hz,
            set[i].rot, set[i].x, set[i].y, set[i].primary, r, DispChangeName(r));
        if (r != DISP_CHANGE_SUCCESSFUL) {
            if (err) _snwprintf_s(err, cch, _TRUNCATE, L"%s: %s (%ld)", set[i].gdi, DispChangeName(r), r);
            return r;
        }
    }
    for (i = 0; i < n; i++) {
        FillDevmode(&dm, &set[i]);
        r = ChangeDisplaySettingsExW(set[i].gdi, &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | (set[i].primary ? CDS_SET_PRIMARY : 0), NULL);
        Log(L"display: stage %s -> %ld (%s)", set[i].gdi, r, DispChangeName(r));
        if (r != DISP_CHANGE_SUCCESSFUL) {
            if (err) _snwprintf_s(err, cch, _TRUNCATE, L"%s: %s (%ld)", set[i].gdi, DispChangeName(r), r);
            return r;
        }
    }
    r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    Log(L"display: apply -> %ld (%s)", r, DispChangeName(r));
    if (r != DISP_CHANGE_SUCCESSFUL && err) _snwprintf_s(err, cch, _TRUNCATE, L"apply: %s (%ld)", DispChangeName(r), r);
    return r;
}

BOOL Dc_Snapshot(DcSnap *snap)
{
    DcState s;
    int k;
    ZeroMemory(snap, sizeof(*snap));
    if (QueryCcd(QDC_ONLY_ACTIVE_PATHS, &snap->np, &snap->p, &snap->nm, &snap->m) != ERROR_SUCCESS) { snap->p = NULL; snap->m = NULL; snap->np = snap->nm = 0; }
    if (Dc_Query(&s)) {
        for (k = 0; k < s.n && snap->ngdi < DC_MAXMON; k++) {
            int j, dup = 0;
            if (!s.m[k].active || !s.m[k].gdi[0]) continue;
            for (j = 0; j < snap->ngdi; j++) if (!_wcsicmp(snap->gdi[j], s.m[k].gdi)) dup = 1;
            if (dup) continue;
            wcscpy_s(snap->gdi[snap->ngdi], 32, s.m[k].gdi);
            snap->dm[snap->ngdi].dmSize = sizeof(DEVMODEW);
            if (!EnumDisplaySettingsW(s.m[k].gdi, ENUM_CURRENT_SETTINGS, &snap->dm[snap->ngdi])) continue;
            snap->prim[snap->ngdi] = s.m[k].primary;
            snap->ngdi++;
        }
    }
    Log(L"display: snapshot paths=%u modes=%u gdi=%d", snap->np, snap->nm, snap->ngdi);
    return snap->np > 0 || snap->ngdi > 0;
}

LONG Dc_Restore(const DcSnap *snap)
{
    LONG rc = ERROR_INVALID_DATA;
    int i;
    if (snap->np) {
        rc = SetDisplayConfig(snap->np, snap->p, snap->nm, snap->m, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        Log(L"display: restore SetDisplayConfig(supplied, save) rc=%ld", rc);
        if (rc == ERROR_SUCCESS) return rc;
        rc = SetDisplayConfig(snap->np, snap->p, snap->nm, snap->m, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        Log(L"display: restore SetDisplayConfig(supplied) rc=%ld", rc);
        if (rc == ERROR_SUCCESS) return rc;
    }
    if (snap->ngdi) {   /* fallback: GDI modes of the sources that were active */
        LONG r = DISP_CHANGE_SUCCESSFUL;
        for (i = 0; i < snap->ngdi; i++) {
            DEVMODEW dm = snap->dm[i];
            dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION | DM_DISPLAYFREQUENCY | DM_DISPLAYORIENTATION;
            r = ChangeDisplaySettingsExW(snap->gdi[i], &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET | (snap->prim[i] ? CDS_SET_PRIMARY : 0), NULL);
            Log(L"display: restore (GDI) stage %s -> %ld", snap->gdi[i], r);
        }
        r = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
        Log(L"display: restore (GDI) apply -> %ld", r);
        rc = r == DISP_CHANGE_SUCCESSFUL ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
    }
    return rc;
}

void Dc_FreeSnap(DcSnap *snap)
{
    free(snap->p); free(snap->m);
    ZeroMemory(snap, sizeof(*snap));
}
