/*
 * mypeui.h - shared declarations of MyPEUI.exe (own code, Win32/GDI only).
 *   MyPEUI.c      entry point, shutdown dialog (spec 6), ms-settings handler (spec 8)
 *   display.c     "Display" window (spec 5)
 *   dispcfg.c     display configuration helpers (QueryDisplayConfig / SetDisplayConfig / ChangeDisplaySettingsEx)
 *   brightness.c  reusable brightness module (WMI root\wmi + DDC/CI via dxva2), also for the clock panel (spec 4)
 */
#ifndef MYPEUI_H
#define MYPEUI_H

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#define MYPE_INI       L"X:\\MyPE\\config\\mypeui.ini"
#define MYPE_DISPJSON  L"X:\\MyPE\\config\\display.json"
#define MYPE_LOGDIR    L"X:\\MyPE\\logs"
#define MYPE_LOGFILE   L"X:\\MyPE\\logs\\mypeui.log"
#define MYPE_MSG_TITLE L"MyPE"

/* message the WinXShell desktop window (class Progman) understands: clean exit, MyPEStart restarts the shell
   (WinXShell patch 07, desktop/desktop.h PM_MYPE_RESTART_SHELL) */
#define MYPE_PM_RESTART_SHELL (WM_APP + 0x30)

void Log(const wchar_t *fmt, ...);
void Message(const wchar_t *fmt, ...);
UINT WindowDpi(HWND h);
void ForceForeground(HWND h);

/* display.c: "Display" window. uri may be NULL (ms-settings:display / ms-settings:screenrotation). */
int DisplayWindow(const wchar_t *uri, int argc, wchar_t **argv);

/* net.c (spec 14): "net on|off|toggle|status" (argv[2] = verb), tray icon, "Network" window (uri may be NULL) */
int NetCommand(int argc, wchar_t **argv);
int NetTray(void);
int NetWindow(const wchar_t *uri);

/* vol.c: volume tray icon ("tray-vol") */
int VolTray(void);

#endif
