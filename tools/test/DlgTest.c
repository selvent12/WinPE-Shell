/*
 * DlgTest.exe - minimal reproduction of the Open/Save dialog problem in MyPE (NEXT_WINXSHELL.md B). Own code, MSVC /MT.
 *   DlgTest.exe <mode> <logfile>
 *   mode: ifd    IFileOpenDialog::Show (modern dialog)
 *         gofn   GetOpenFileNameW, Explorer style (ends up in the modern dialog on current Windows)
 *         hook   GetOpenFileNameW + OFN_ENABLEHOOK without OFN_EXPLORER (forces the old Win3.x style dialog)
 *         hookx  GetOpenFileNameW + OFN_EXPLORER + OFN_ENABLEHOOK (legacy Explorer-style dialog, not IFileDialog)
 *         sbff   SHBrowseForFolderW
 * A watcher thread logs when a dialog window of this process becomes visible, then closes it (IDCANCEL) after 2 s,
 * so the main thread returns. If nothing appears, the caller (test action) dumps the hung process and kills it.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static HANDLE g_log = INVALID_HANDLE_VALUE;
static DWORD g_t0;
static volatile LONG g_seen = 0;

static void Log(const wchar_t *fmt, ...)
{
    wchar_t msg[800], line[900]; char u8[2800]; va_list ap; DWORD n; int len;
    va_start(ap, fmt); _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap); va_end(ap);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"[%6lu ms] %s\r\n", GetTickCount() - g_t0, msg);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL);
    if (len > 1 && g_log != INVALID_HANDLE_VALUE) { WriteFile(g_log, u8, (DWORD)(len - 1), &n, NULL); FlushFileBuffers(g_log); }
}

static BOOL CALLBACK FindDlg(HWND h, LPARAM lp)
{
    DWORD pid = 0; wchar_t cls[64];
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"#32770") != 0) return TRUE;   /* dialogs only, not our own console window */
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI Watcher(LPVOID p)
{
    HWND h = NULL; wchar_t cls[64] = L"", title[128] = L"";
    (void)p;
    for (;;) {
        h = NULL; EnumWindows(FindDlg, (LPARAM)&h);
        if (h) {
            GetClassNameW(h, cls, 64); GetWindowTextW(h, title, 128);
            InterlockedExchange(&g_seen, 1);
            Log(L"DIALOG VISIBLE class=%s title='%s'", cls, title);
            Sleep(2000);
            PostMessageW(h, WM_COMMAND, IDCANCEL, 0);
            PostMessageW(h, WM_CLOSE, 0, 0);
            Log(L"cancel sent");
            return 0;
        }
        Sleep(100);
    }
}

static UINT_PTR CALLBACK Hook(HWND h, UINT m, WPARAM w, LPARAM l) { (void)h; (void)m; (void)w; (void)l; return 0; }

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *mode = argc > 1 ? argv[1] : L"ifd";
    HRESULT hr = S_OK; BOOL ok = FALSE; wchar_t file[MAX_PATH] = L"";
    g_t0 = GetTickCount();
    if (argc > 2) g_log = CreateFileW(argv[2], FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    Log(L"DlgTest mode=%s pid=%lu", mode, GetCurrentProcessId());
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Log(L"CoInitializeEx hr=0x%08lx", (unsigned long)hr);
    CloseHandle(CreateThread(NULL, 0, Watcher, NULL, 0, NULL));

    if (_wcsicmp(mode, L"ifd") == 0) {
        IFileOpenDialog *d = NULL;
        hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d);
        Log(L"CoCreateInstance(FileOpenDialog) hr=0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            Log(L"calling Show");
            hr = d->lpVtbl->Show(d, NULL);
            Log(L"Show returned hr=0x%08lx", (unsigned long)hr);
            d->lpVtbl->Release(d);
        }
    } else if (_wcsicmp(mode, L"sbff") == 0) {
        BROWSEINFOW bi; PIDLIST_ABSOLUTE pidl;
        ZeroMemory(&bi, sizeof(bi)); bi.lpszTitle = L"DlgTest SHBrowseForFolder"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        Log(L"calling SHBrowseForFolder");
        pidl = SHBrowseForFolderW(&bi);
        Log(L"SHBrowseForFolder returned %p", (void *)pidl);
        if (pidl) CoTaskMemFree(pidl);
    } else {
        OPENFILENAMEW o;
        ZeroMemory(&o, sizeof(o)); o.lStructSize = sizeof(o); o.lpstrFile = file; o.nMaxFile = MAX_PATH;
        o.lpstrFilter = L"All files\0*.*\0"; o.lpstrInitialDir = L"X:\\";
        if (_wcsicmp(mode, L"gofn") == 0) o.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
        else if (_wcsicmp(mode, L"hook") == 0) { o.Flags = OFN_ENABLEHOOK | OFN_FILEMUSTEXIST; o.lpfnHook = (LPOFNHOOKPROC)Hook; }
        else { o.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_FILEMUSTEXIST; o.lpfnHook = (LPOFNHOOKPROC)Hook; }
        Log(L"calling GetOpenFileNameW flags=0x%08lx", o.Flags);
        ok = GetOpenFileNameW(&o);
        Log(L"GetOpenFileNameW returned %d, CommDlgExtendedError=0x%lx", ok, CommDlgExtendedError());
    }
    Log(L"END mode=%s dialog_seen=%ld", mode, g_seen);
    CoUninitialize();
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return g_seen ? 0 : 3;
}
