/*
 * shellui.h - shared declarations of WinPE-ShellUI.exe (own code, Win32/GDI only).
 *   shellui.c     entry point, shutdown dialog, ms-settings handler, file paths
 *   display.c     "Display" window
 *   dispcfg.c     display configuration helpers (QueryDisplayConfig / SetDisplayConfig / ChangeDisplaySettingsEx)
 *   brightness.c  reusable brightness module (WMI root\wmi + DDC/CI via dxva2)
 *   net.c, scale.c, vol.c   network, live display scale, volume icon
 */
#ifndef WPS_SHELLUI_H
#define WPS_SHELLUI_H

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

/* file locations, relative to the folder of WinPE-ShellUI.exe (no fixed drive or folder):
     <exe dir>\config\winpe-shellui.ini   (the former name mypeui.ini is still read when the new one is missing)
     <exe dir>\config\display.json, <exe dir>\logs\winpe-shellui.log, <exe dir>\logs\net-commands.log */
enum { WPS_P_DIR, WPS_P_INI, WPS_P_DISPJSON, WPS_P_LOGDIR, WPS_P_LOGFILE, WPS_P_CMDLOG, WPS_P_COUNT };
const wchar_t *WpsPath(int which);
#define WPS_INI       WpsPath(WPS_P_INI)
#define WPS_DISPJSON  WpsPath(WPS_P_DISPJSON)
#define WPS_LOGDIR    WpsPath(WPS_P_LOGDIR)
#define WPS_LOGFILE   WpsPath(WPS_P_LOGFILE)
#define WPS_CMDLOG    WpsPath(WPS_P_CMDLOG)
#define WPS_MSG_TITLE L"WinPE-Shell"

/* message the shell's desktop window (class Progman) understands: clean exit, WinPE-ShellStart restarts the shell
   (desktop/desktop.h PM_WPS_RESTART_SHELL) */
#define WPS_PM_RESTART_SHELL (WM_APP + 0x30)

void Log(const wchar_t *fmt, ...);
void Message(const wchar_t *fmt, ...);
UINT WindowDpi(HWND h);
void ForceForeground(HWND h);

/* display.c: "Display" window. uri may be NULL (ms-settings:display / ms-settings:screenrotation). */
int DisplayWindow(const wchar_t *uri, int argc, wchar_t **argv);

/* net.c: "net on|off|toggle|status" (argv[2] = verb), tray icon, "Network" window (uri may be NULL) */
int NetCommand(int argc, wchar_t **argv);
int NetTray(void);
int NetWindow(const wchar_t *uri);

/* vol.c: volume tray icon ("tray-vol") */
int VolTray(void);

/* calendar.c: calendar flyout above the clock ("calendar") */
int CalendarWindow(void);

/* snip.c: Snipping-Tool-like window ("snip", "snip /now": snip at once) */
int SnipCommand(int argc, wchar_t **argv);

/* dim.c: screen dimming without brightness control ("dim <percent>"; gamma ramp, else a layer window up to 10 MB) */
int DimCommand(int argc, wchar_t **argv);
BOOL Dim_Available(void);
int Dim_Current(void);
void Dim_Set(int pct);

/* bde.c: unlock BitLocker drives ("bitlocker [X:] [/check]") */
int BitLockerCommand(int argc, wchar_t **argv);

/* bat.c: battery tray icon ("tray-bat"; ends at once without a battery) */
int BatTray(void);

#endif
