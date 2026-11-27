/*
 * brightness.h - reusable brightness module (README.md). Own code.
 *   1. laptop panels: WMI root\wmi  WmiMonitorBrightness (read) / WmiMonitorBrightnessMethods.WmiSetBrightness (write)
 *   2. external monitors: Microsoft monitor configuration API (dxva2.dll, DDC/CI), loaded at run time (optional in PE)
 * All functions are synchronous and can be slow (WMI start, DDC/CI bus): call them from a worker thread.
 * The calling thread must have COM initialised (CoInitializeEx) for the WMI targets; Bri_Probe does it itself
 * if needed and leaves it initialised for the thread.
 */
#ifndef BRIGHTNESS_H
#define BRIGHTNESS_H
#include "shellui.h"

#define BRI_MAX 8
enum { BRI_WMI = 1, BRI_DDC = 2 };

typedef struct {
    int kind;                  /* BRI_WMI / BRI_DDC */
    HMONITOR hmon;             /* DDC: the display monitor; WMI: NULL (the internal panel) */
    wchar_t gdi[32];           /* DDC: \\.\DISPLAYn of hmon */
    HANDLE phys;               /* DDC: physical monitor handle */
    wchar_t desc[128];
    wchar_t instance[256];     /* WMI: InstanceName */
    DWORD minv, maxv;          /* DDC raw range */
    BYTE levels[101];          /* WMI: supported levels (percent) */
    int nlevels;
} BriTarget;

int  Bri_Probe(BriTarget *t, int max);          /* number of controllable targets found (0 = none: hide the UI) */
int  Bri_GetPercent(BriTarget *t);              /* 0..100, -1 on failure */
BOOL Bri_SetPercent(BriTarget *t, int pct);
void Bri_Free(BriTarget *t, int n);
const wchar_t *Bri_KindName(int kind);

#endif
