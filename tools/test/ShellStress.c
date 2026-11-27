/*
 * ShellStress.exe - shell stability stress test for WinPE. Own code, no dependencies, MSVC /MT.
 * Runs INSIDE the test PE only. For N minutes: opens/closes windows of notepad, regedit, cmd, Explorer++,
 * switches/minimises/restores them, opens the Start menu (Ctrl+Esc), and checks that the shell (owner of
 * Shell_TrayWnd) keeps the same PID. Log: argv[2] (UTF-8). Exit code 0 = shell PID unchanged, 1 = changed/missing.
 *   ShellStress.exe <minutes> <logfile>
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void Log(const wchar_t *fmt, ...)
{
    wchar_t msg[1024], line[1200];
    char u8[3600];
    SYSTEMTIME t;
    va_list ap;
    DWORD n;
    int len;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    GetLocalTime(&t);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02u:%02u:%02u %s\r\n", t.wHour, t.wMinute, t.wSecond, msg);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    if (len > 1 && g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, u8, (DWORD)(len - 1), &n, NULL);
}

static DWORD ShellPid(void)
{
    DWORD pid = 0;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (tray) GetWindowThreadProcessId(tray, &pid);
    return pid;
}

typedef struct { DWORD pid; HWND hwnd; } FINDWND;
static BOOL CALLBACK FindTop(HWND h, LPARAM lp)
{
    FINDWND *f = (FINDWND *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) { f->hwnd = h; return FALSE; }
    return TRUE;
}

/* console windows belong to conhost, not to cmd: find by title prefix of our own console title */
static HWND WaitWindow(DWORD pid, const wchar_t *title, DWORD ms)
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        if (title) { HWND h = FindWindowW(L"ConsoleWindowClass", title); if (h) return h; }
        else { FINDWND f = { pid, NULL }; EnumWindows(FindTop, (LPARAM)&f); if (f.hwnd) return f.hwnd; }
        Sleep(100);
    }
    return NULL;
}

#define MAXW 8
typedef struct { HWND hwnd; HANDLE proc; } OPENW;

static void CloseOne(OPENW *w)
{
    if (w->hwnd && IsWindow(w->hwnd)) PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
    if (w->proc) {
        if (WaitForSingleObject(w->proc, 5000) == WAIT_TIMEOUT) { TerminateProcess(w->proc, 1); Log(L"  force-terminated a window process"); }
        CloseHandle(w->proc);
    }
    w->hwnd = NULL; w->proc = NULL;
}

static void Key(WORD vk, BOOL up)
{
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

int wmain(int argc, wchar_t **argv)
{
    int minutes = argc > 1 ? _wtoi(argv[1]) : 30;
    const wchar_t *apps[] = {
        L"X:\\Windows\\System32\\notepad.exe",
        L"X:\\Windows\\regedit.exe",
        L"X:\\Windows\\System32\\cmd.exe",
        L"X:\\Program Files\\Explorer++\\Explorer++.exe"
    };
    OPENW open[MAXW];
    DWORD shell0, t0, opened = 0, failed = 0, switches = 0, starts = 0, cycle = 0, shellLost = 0;
    int i;

    if (argc > 2) g_log = CreateFileW(argv[2], FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    ZeroMemory(open, sizeof(open));
    shell0 = ShellPid();
    Log(L"ShellStress start: %d min, shell pid %lu", minutes, shell0);
    t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)minutes * 60000u) {
        int slot = (int)(cycle % MAXW);
        const wchar_t *exe = apps[cycle % _countof(apps)];
        STARTUPINFOW si; PROCESS_INFORMATION pi; wchar_t cmd[600], title[64];
        BOOL isCmd = wcsstr(exe, L"cmd.exe") != NULL;

        if (open[slot].proc || open[slot].hwnd) CloseOne(&open[slot]);
        ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        _snwprintf_s(title, _countof(title), _TRUNCATE, L"ShellStress-%lu", cycle);
        if (isCmd) { si.lpTitle = title; _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" /k echo stress", exe); }
        else _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\"", exe);
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, isCmd ? CREATE_NEW_CONSOLE : 0, NULL, L"X:\\", &si, &pi)) {
            CloseHandle(pi.hThread);
            open[slot].proc = pi.hProcess;
            open[slot].hwnd = WaitWindow(pi.dwProcessId, isCmd ? title : NULL, 8000);
            if (open[slot].hwnd) opened++; else { failed++; Log(L"  no window for %s (pid %lu)", exe, pi.dwProcessId); }
        } else { failed++; Log(L"  CreateProcess failed %lu: %s", GetLastError(), exe); }

        /* switch / minimise / restore across the open windows */
        for (i = 0; i < MAXW; i++) {
            if (!open[i].hwnd || !IsWindow(open[i].hwnd)) continue;
            if ((cycle + i) % 3 == 0) { ShowWindow(open[i].hwnd, SW_MINIMIZE); Sleep(50); ShowWindow(open[i].hwnd, SW_RESTORE); }
            SetForegroundWindow(open[i].hwnd); switches++;
            Sleep(40);
        }
        /* Start menu every 5 cycles: Ctrl+Esc, then Esc */
        if (cycle % 5 == 4) { Key(VK_CONTROL, FALSE); Key(VK_ESCAPE, FALSE); Key(VK_ESCAPE, TRUE); Key(VK_CONTROL, TRUE); Sleep(600); Key(VK_ESCAPE, FALSE); Key(VK_ESCAPE, TRUE); starts++; }

        if (ShellPid() != shell0) { shellLost++; Log(L"  !! shell pid changed: %lu -> %lu", shell0, ShellPid()); shell0 = ShellPid(); }
        if (cycle % 50 == 0) Log(L"cycle %lu: opened %lu failed %lu switches %lu start %lu shell %lu", cycle, opened, failed, switches, starts, ShellPid());
        cycle++;
        Sleep(300);
    }
    for (i = 0; i < MAXW; i++) CloseOne(&open[i]);
    Log(L"ShellStress end: cycles %lu, windows opened %lu, failed %lu, switches %lu, start menu %lu, shell pid changes %lu, final shell pid %lu",
        cycle, opened, failed, switches, starts, shellLost, ShellPid());
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return shellLost == 0 && ShellPid() != 0 ? 0 : 1;
}
