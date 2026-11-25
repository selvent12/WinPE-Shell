/*
 * scale.h - live display scale (per monitor, no new session), as the Settings app changes it.
 * VM test 2026-11-25 (WinPE 26H2): the effective DPI changes at once and processes started afterwards get the new
 * system DPI, so a shell restart is enough for a sharp taskbar.
 */
#pragma once
#include <windows.h>

#define SCALE_MAXSTEPS 12

/* percent values Windows uses, in order (index = step) */
extern const int g_scaleSteps[SCALE_MAXSTEPS];

/* current scale of the main display (percent) and the largest scale any active display accepts; FALSE if unknown */
BOOL Scale_Query(int *curPct, int *maxPct);
/* sets every active display to pct (clamped to what each display accepts); *changed = a display changed.
   Returns FALSE if a display could not be set (details in the log). */
BOOL Scale_SetAll(int pct, BOOL *changed);
/* nearest valid step for a percent value (100..500) */
int Scale_Nearest(int pct);
