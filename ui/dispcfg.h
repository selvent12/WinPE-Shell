/*
 * dispcfg.h - display configuration helpers for the "Display" window (README.md).
 * Same approach as builder\tools\Set-DisplayLayout.ps1 / DisplayLayout.cs, rewritten in C:
 *   topology (Extend / Duplicate / only N) and snapshot / revert: QueryDisplayConfig + SetDisplayConfig (CCD API);
 *   per-monitor resolution / refresh / rotation / primary / position: EnumDisplaySettings + ChangeDisplaySettingsEx.
 */
#ifndef DISPCFG_H
#define DISPCFG_H
#include "shellui.h"

#define DC_MAXMON   8
#define DC_MAXMODES 192

enum { DC_TOPO_NONE = 0, DC_TOPO_SINGLE, DC_TOPO_EXTEND, DC_TOPO_CLONE, DC_TOPO_ONLY };

typedef struct { DWORD w, h, hz; } DcMode;      /* unrotated (landscape as the panel reports it) */

typedef struct {
    LUID adapter;
    UINT32 targetId;
    wchar_t name[64];          /* monitor friendly name (EDID), or "Display N" */
    wchar_t adapterName[128];  /* EnumDisplayDevices DeviceString, e.g. "Microsoft Basic Display Driver" */
    wchar_t gdi[32];           /* \\.\DISPLAYn, only when active */
    BOOL active, primary, internal;
    UINT32 sourceId;
    int x, y;                  /* desktop position (active only) */
    DWORD w, h;                /* desktop size = rotated (active only) */
    DWORD nw, nh;              /* current mode, unrotated */
    DWORD hz;
    int rot;                   /* 0, 90, 180, 270 */
    DWORD prefW, prefH;        /* preferred (native) mode, 0 if unknown */
    int nmodes;
    DcMode modes[DC_MAXMODES]; /* sorted: largest first, then refresh descending */
} DcMonitor;

typedef struct {
    int n;                     /* connected monitors (targets available) */
    DcMonitor m[DC_MAXMON];
    int nactive, nsources;
    int topo;                  /* DC_TOPO_* */
    int only;                  /* DC_TOPO_ONLY: index of the active monitor */
} DcState;

/* one active GDI source to (re)configure; w/h unrotated */
typedef struct { const wchar_t *gdi; DWORD w, h, hz; int rot; int x, y; BOOL primary; } DcSet;

/* revert point: active CCD paths + modes, plus the GDI current modes as a fallback */
typedef struct {
    UINT32 np, nm;
    DISPLAYCONFIG_PATH_INFO *p;
    DISPLAYCONFIG_MODE_INFO *m;
    int ngdi;
    wchar_t gdi[DC_MAXMON][32];
    DEVMODEW dm[DC_MAXMON];
    BOOL prim[DC_MAXMON];
} DcSnap;

BOOL Dc_Query(DcState *s);                         /* FALSE: QueryDisplayConfig failed (s->n = 0) */
void Dc_Describe(const DcState *s, wchar_t *buf, size_t cch);
LONG Dc_SetTopology(const DcState *s, int topo, int only);   /* ERROR_SUCCESS or a Win32 error */
LONG Dc_ApplyGdi(const DcSet *set, int n, wchar_t *err, size_t cch);   /* DISP_CHANGE_SUCCESSFUL (0) or DISP_CHANGE_* */
BOOL Dc_Snapshot(DcSnap *snap);
LONG Dc_Restore(const DcSnap *snap);
void Dc_FreeSnap(DcSnap *snap);
const wchar_t *Dc_TopoName(int topo);

#endif
