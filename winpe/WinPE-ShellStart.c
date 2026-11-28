/*
 * WinPE-ShellStart.exe - WinPE startup launcher and shell watchdog (WinPE-Shell, winpe/). Own code, GNU LGPL 2.1 or later, no third-party parts.
 * GUI subsystem (no console window), static CRT. Started by winpeshl.ini as the ONLY LaunchApps entry and never exits
 * while PE runs (winpeshl reboots PE when its last app ends).
 *
 *  1. wpeinit (-unattend from the ini), CREATE_NO_WINDOW, waits
 *  2. [run] steps from the ini, hidden (CREATE_NO_WINDOW + SW_HIDE), "wait|" or "nowait|" prefix
 *  3. starts the shell (ini [shell] path/args)
 *  4. watchdog: restarts the shell; more than 3 crashes within 60 s -> visible cmd.exe fallback (re-opened if closed);
 *     requested restarts (exit code 0x7F04 or 0x7F05: "Restart shell", live scale change) have their own limit, 10 within 60 s;
 *     a HUNG shell (its Shell_TrayWnd does not answer WM_NULL for ~2 min) is ended (rc 0x7F06) and counted as a crash
 *  5. log: <exe dir>\logs\startup.log
 *
 * Config <exe dir>\config\startup.ini (written by the image builder), for example with the exe in X:\MyPE:
 *   [startup]
 *   wpeinit=X:\Windows\System32\wpeinit.exe -unattend:X:\MyPE\config\Unattend.xml
 *   [run]
 *   1=wait|X:\Windows\System32\cmd.exe /d /c md "X:\ProgramData\Microsoft\Windows\AppRepository"
 *   2=nowait|X:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File X:\MyPE\bin\Set-DisplayLayout.ps1
 *   [shell]
 *   name=winpe-shell
 *   path=X:\Program Files\WinPE-Shell\WinPE-Shell.exe
 *   args=-winpe
 *   [env]
 *   USERPROFILE=X:\Users\Default
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

#define INI      g_ini       /* <exe dir>\config\startup.ini (InitPaths) */
#define LOGDIR   g_logdir    /* <exe dir>\logs */
#define LOGFILE  g_logfile   /* <exe dir>\logs\startup.log */
#define MAXCMD   4096

static wchar_t g_ini[MAX_PATH], g_logdir[MAX_PATH], g_logfile[MAX_PATH];

/* file locations relative to the exe folder (no fixed X:\MyPE); called first in wWinMain (single thread) */
static void InitPaths(void)
{
    wchar_t dir[MAX_PATH], *slash;
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH) || !(slash = wcsrchr(dir, L'\\'))) wcscpy_s(dir, MAX_PATH, L"X:");
    else slash[0] = 0;
    _snwprintf_s(g_ini, MAX_PATH, _TRUNCATE, L"%s\\config\\startup.ini", dir);
    _snwprintf_s(g_logdir, MAX_PATH, _TRUNCATE, L"%s\\logs", dir);
    _snwprintf_s(g_logfile, MAX_PATH, _TRUNCATE, L"%s\\logs\\startup.log", dir);
}

static void Log(const wchar_t *fmt, ...)
{
    wchar_t msg[2048], line[2300];
    SYSTEMTIME t;
    va_list ap;
    HANDLE h;
    DWORD n;
    char utf8[7000];
    int len;

    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    GetLocalTime(&t);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u WinPE-ShellStart %s\r\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 1) return;
    CreateDirectoryW(LOGDIR, NULL);
    h = CreateFileW(LOGFILE, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, utf8, (DWORD)(len - 1), &n, NULL);
    CloseHandle(h);
}

/* Starts cmdline. hidden: no window at all. Returns process handle (caller closes) or NULL. */
static HANDLE Start(const wchar_t *cmdline, BOOL hidden, const wchar_t *dir)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t buf[MAXCMD];

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    wcsncpy_s(buf, _countof(buf), cmdline, _TRUNCATE);   /* CreateProcessW may modify the buffer */
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, hidden ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
        Log(L"start FAILED err=%lu: %s", GetLastError(), cmdline);
        return NULL;
    }
    CloseHandle(pi.hThread);
    Log(L"started pid=%lu%s: %s", pi.dwProcessId, hidden ? L" (hidden)" : L"", cmdline);
    return pi.hProcess;
}

static void RunAndWait(const wchar_t *label, const wchar_t *cmdline, DWORD timeoutMs)
{
    DWORD t0 = GetTickCount(), rc = 0, w;
    HANDLE p = Start(cmdline, TRUE, NULL);
    if (!p) return;
    w = WaitForSingleObject(p, timeoutMs);
    if (w == WAIT_TIMEOUT) Log(L"%s still running after %lu ms - continuing", label, timeoutMs);
    else { GetExitCodeProcess(p, &rc); Log(L"%s exited rc=%lu (0x%08lX) after %lu ms", label, rc, rc, GetTickCount() - t0); }
    CloseHandle(p);
}

static void ApplyEnv(void)
{
    wchar_t sect[8192], *k, val[MAXCMD], exp[MAXCMD];
    DWORD n = GetPrivateProfileSectionW(L"env", sect, _countof(sect), INI);
    if (!n) return;
    for (k = sect; *k; k += wcslen(k) + 1) {
        wchar_t *eq = wcschr(k, L'=');
        if (!eq) continue;
        *eq = 0;
        wcsncpy_s(val, _countof(val), eq + 1, _TRUNCATE);
        ExpandEnvironmentStringsW(val, exp, _countof(exp));
        SetEnvironmentVariableW(k, exp);
        Log(L"env %s=%s", k, exp);
        *eq = L'=';
    }
}

static void RunSteps(void)
{
    wchar_t key[16], val[MAXCMD];
    int i;
    for (i = 1; i <= 64; i++) {
        _snwprintf_s(key, _countof(key), _TRUNCATE, L"%d", i);
        if (!GetPrivateProfileStringW(L"run", key, L"", val, _countof(val), INI)) continue;
        if (_wcsnicmp(val, L"wait|", 5) == 0) RunAndWait(key, val + 5, 120000);
        else if (_wcsnicmp(val, L"wait", 4) == 0 && iswdigit(val[4])) {
            /* "wait<seconds>|cmd": own timeout (drvload over many oem*.inf can take longer than 120 s, and the next
               step "net off /boot" must not run while drivers are still being installed - review 2026-11-26) */
            wchar_t *bar = wcschr(val, L'|');
            unsigned long sec = wcstoul(val + 4, NULL, 10);
            if (bar && sec > 0 && sec <= 3600) RunAndWait(key, bar + 1, sec * 1000);
            else Log(L"run.%s ignored (bad wait<seconds>| prefix): %s", key, val);
        }
        else if (_wcsnicmp(val, L"nowait|", 7) == 0) { HANDLE p = Start(val + 7, TRUE, NULL); if (p) CloseHandle(p); }
        else Log(L"run.%s ignored (needs wait|, wait<seconds>| or nowait| prefix): %s", key, val);
    }
}

/* Hang check: the shell's taskbar window (Shell_TrayWnd of that process) must answer WM_NULL within 5 s. Returns 1
   answered, 0 no answer, -1 no taskbar window of that process yet (still starting, or a shell without one). */
#define HANG_CHECK_MS   5000     /* wait between checks = SendMessageTimeout limit */
#define HANG_LIMIT      12       /* 12 failed checks in a row (~2 min) -> the shell is ended and restarted */
static int ShellResponds(HANDLE shell)
{
    DWORD pid = GetProcessId(shell), wpid = 0;
    HWND w = NULL;
    DWORD_PTR r;
    while ((w = FindWindowExW(NULL, w, L"Shell_TrayWnd", NULL)) != NULL) {
        GetWindowThreadProcessId(w, &wpid);
        if (wpid == pid) break;
    }
    if (!w) return -1;
    return SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, HANG_CHECK_MS, &r) ? 1 : 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    wchar_t wpeinit[MAXCMD], name[64], path[MAX_PATH], args[MAXCMD], shellCmd[MAXCMD], cmdexe[MAX_PATH], dir[MAX_PATH];
    DWORD restarts[4] = {0, 0, 0, 0};
    /* requested restarts ("Restart shell", a live scale change: exit code 0x7F04, or 0x7F05 from the shell's 8 s exit
       guard) are NOT crashes: counted separately with a much higher limit (10 within 60 s), so a user who changes the
       scale a few times in a row keeps the shell; crashes keep the 4-starts-in-60-s limit */
    DWORD clean[10] = {0};
    DWORD lastRc = 0xFFFFFFFF;
    int nrest = 0, nclean = 0, nstart = 0, fallback = 0, hung = 0;
    HANDLE shell = NULL, rescue = NULL;
    (void)hInst; (void)hPrev; (void)cmdLine; (void)nShow;

    InitPaths();
    Log(L"=== begin (ini %s)", INI);
    if (GetFileAttributesW(INI) == INVALID_FILE_ATTRIBUTES) Log(L"WARNING ini missing - defaults: wpeinit, explorer");

    ApplyEnv();
    GetPrivateProfileStringW(L"startup", L"wpeinit", L"X:\\Windows\\System32\\wpeinit.exe", wpeinit, _countof(wpeinit), INI);
    if (wpeinit[0]) RunAndWait(L"wpeinit", wpeinit, 300000);

    RunSteps();

    GetPrivateProfileStringW(L"shell", L"name", L"explorer", name, _countof(name), INI);
    GetPrivateProfileStringW(L"shell", L"path", L"X:\\Windows\\explorer.exe", path, _countof(path), INI);
    GetPrivateProfileStringW(L"shell", L"args", L"", args, _countof(args), INI);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        Log(L"shell %s not found: %s -> fallback cmd", name, path);
        fallback = 1;
    }
    _snwprintf_s(shellCmd, _countof(shellCmd), _TRUNCATE, L"\"%s\"%s%s", path, args[0] ? L" " : L"", args);
    wcsncpy_s(dir, _countof(dir), path, _TRUNCATE);
    { wchar_t *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\cmd.exe", cmdexe, _countof(cmdexe));

    for (;;) {
        if (!fallback && !shell) {
            DWORD now = GetTickCount();
            /* WinPE-Shell: 0x7F04 = requested restart (clean exit), 0x7F05 = its 8 s exit guard; VM 2026-11-25: the
               plain clean exit returned 1, so a generic code cannot be trusted */
            BOOL requested = lastRc == 0x7F04 || lastRc == 0x7F05;
            BOOL giveUp;
            if (requested) {
                clean[nclean++ % 10] = now;
                giveUp = nclean >= 10 && now - clean[nclean % 10] < 60000;   /* 10 clean exits within 60 s */
            } else {
                restarts[nrest++ % 4] = now;
                giveUp = nrest >= 4 && now - restarts[nrest % 4] < 60000;     /* 4 starts (= 3 crashes) within 60 s */
            }
            if (giveUp) {
                Log(L"shell %s %s within 60 s -> fallback cmd", name, requested ? L"exited 10 times (requested restarts)" : L"failed 3 times");
                fallback = 1;
            } else {
                if (requested) Log(L"requested shell restart (rc=%lu, %d in a row)", lastRc, nclean);
                Log(L"starting shell %s (start #%d)", name, ++nstart);   /* all starts, requested restarts included */
                shell = Start(shellCmd, FALSE, dir);
                if (!shell) Sleep(2000);
            }
        }
        if (fallback && !rescue) {
            wchar_t c[MAX_PATH + 64];
            _snwprintf_s(c, _countof(c), _TRUNCATE, L"\"%s\" /k echo WinPE-Shell: shell not running - see %s", cmdexe, LOGFILE);
            rescue = Start(c, FALSE, L"X:\\");
            if (!rescue) Sleep(5000);
        }
        {
            HANDLE hs[2]; DWORD n = 0, w, rc = 0;
            if (shell) hs[n++] = shell;
            if (rescue) hs[n++] = rescue;
            if (!n) continue;
            w = WaitForMultipleObjects(n, hs, FALSE, shell ? HANG_CHECK_MS : INFINITE);
            if (w == WAIT_TIMEOUT) {
                /* the shell runs: is it hung? (a crash ends the process, a hang does not - review 2026-11-27) */
                int ok = ShellResponds(shell);
                if (ok == 0) {
                    if (++hung == 1 || hung % 4 == 0) Log(L"shell %s not responding (%d check(s), %d s)", name, hung, hung * HANG_CHECK_MS * 2 / 1000);
                    if (hung >= HANG_LIMIT) {
                        Log(L"shell %s hung for ~%d s -> ended (rc 0x7F06), restart counted as a crash", name, hung * HANG_CHECK_MS * 2 / 1000);
                        TerminateProcess(shell, 0x7F06);
                        hung = 0;   /* the wait above sees the process end on the next pass */
                    }
                } else {
                    if (hung) Log(L"shell %s responding again after %d failed check(s)", name, hung);
                    hung = 0;
                }
                continue;
            }
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + n) {
                HANDLE ended = hs[w - WAIT_OBJECT_0];
                GetExitCodeProcess(ended, &rc);
                if (ended == shell) { Log(L"shell %s exited rc=%lu (0x%08lX)", name, rc, rc); CloseHandle(shell); shell = NULL; lastRc = rc; hung = 0; }
                else { Log(L"fallback cmd exited rc=%lu - reopening", rc); CloseHandle(rescue); rescue = NULL; }
            } else {
                Log(L"wait failed err=%lu", GetLastError());
                Sleep(1000);
            }
        }
    }
    /* not reached */
}
