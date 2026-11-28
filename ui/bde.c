/*
 * bde.c - unlock BitLocker drives (README.md). Own code; idea from PExplorer/WinXShell (f7f8359 / 2f3bb92: unlock a
 * locked drive when it is opened).
 *
 *   WinPE-ShellUI.exe bitlocker [X:]      window: locked drive(s), password or 48-digit recovery key, Unlock;
 *                                         "Key file..." = a startup key file (.BEK) on a stick
 *   WinPE-ShellUI.exe bitlocker /check    the same, but only if a locked drive exists (start-up step), else nothing
 *
 * Locked drives: shell property System.Volume.BitLockerProtection = 6 (what Windows' own "Unlock Drive..." verb
 * tests), else (WinPE) the volume's first sector cannot be read (FVE_E_LOCKED_VOLUME). Unlocking: BitLocker's WMI
 * class Win32_EncryptableVolume (WinPE-WMI + WinPE-SecureStartup) through PowerShell, UnlockWithPassphrase /
 * UnlockWithNumericalPassword / GetExternalKeyFromFile + UnlockWithExternalKey. Drive and secret go to PowerShell on
 * its standard input, never on a command line and never into a log.
 * Key information (GetKeyProtectors / GetKeyProtectorType): which ways unlock the drive and the recovery key ID, the
 * one listed in the Microsoft account / company portal and on Windows' blue recovery screen. Drives encrypted by
 * Windows itself (device encryption, company policy) only have TPM + recovery key: the TPM does not unlock in PE.
 */
#include "shellui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <propsys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BDE_CLASS   L"WinPE-Shell.BitLocker"
#define WM_APP_DONE (WM_APP + 1)
#define WM_APP_INFO (WM_APP + 2)    /* lParam = HeapAlloc'd text for the info line, wParam = request generation */
enum { IDC_B_DRIVE = 401, IDC_B_KEY, IDC_B_SHOW, IDC_B_UNLOCK, IDC_B_CLOSE, IDC_B_STATUS, IDC_B_L1, IDC_B_L2,
       IDC_B_INFO, IDC_B_FILE };

static struct {
    HWND wnd;
    HFONT font;
    double s;
    BOOL busy;
    wchar_t drive[4];
    wchar_t key[300];               /* password / recovery key, or "keyfile:<path>" */
    volatile LONG infoGen;          /* newest key information request (older answers are dropped) */
} B;

static int BS(double v) { return (int)(v * B.s + 0.5); }

/* 6 = locked (see the top); 0 = no BitLocker / unknown */
static int Protection(const wchar_t *root)
{
    IShellItem2 *item = NULL;
    PROPERTYKEY pk;
    PROPVARIANT pv;
    int st = 0;
    if (FAILED(PSGetPropertyKeyFromName(L"System.Volume.BitLockerProtection", &pk))) return 0;
    if (FAILED(SHCreateItemFromParsingName(root, NULL, &IID_IShellItem2, (void **)&item))) return 0;
    PropVariantInit(&pv);
    if (SUCCEEDED(item->lpVtbl->GetProperty(item, &pk, &pv))) {
        if (pv.vt == VT_I4 || pv.vt == VT_UI4 || pv.vt == VT_INT || pv.vt == VT_UINT) st = pv.intVal;
        PropVariantClear(&pv);
    }
    item->lpVtbl->Release(item);
    return st;
}

/* In WinPE the shell property above is not filled (VM 2026-11-28: a locked stick gave 0). A locked BitLocker volume
   cannot be read at all: reading its first sector fails with FVE_E_LOCKED_VOLUME. */
#define WPS_FVE_LOCKED 0x80310000UL   /* FVE_E_LOCKED_VOLUME (winerror.h, an HRESULT) as the GetLastError value */
static BOOL RawLocked(wchar_t letter)
{
    wchar_t dev[8] = { L'\\', L'\\', L'.', L'\\', letter, L':', 0 };
    HANDLE h = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *buf;
    DWORD n = 0, err = 0;
    BOOL locked = FALSE;
    if (h == INVALID_HANDLE_VALUE) return (DWORD)GetLastError() == WPS_FVE_LOCKED;
    buf = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);   /* sector aligned */
    if (buf && !ReadFile(h, buf, 4096, &n, NULL)) {
        err = GetLastError();
        locked = err == WPS_FVE_LOCKED;
        if (!locked) Log(L"bitlocker: %c: first sector not readable (err 0x%08lX)", letter, err);
    } else if (buf && n >= 11 && !memcmp(buf + 3, "-FVE-FS-", 8))
        locked = TRUE;      /* raw BitLocker header visible = not unlocked */
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    return locked;
}

/* locked drives into the combo box (or count only when combo is NULL) */
static int ListLocked(HWND combo, const wchar_t *want)
{
    DWORD mask = GetLogicalDrives();
    int n = 0, sel = 0, i;
    if (combo) SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (i = 2; i < 26; i++) {      /* C: .. Z: */
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t;
        if (!(mask & (1u << i))) continue;
        t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        if (Protection(root) != 6 && !RawLocked(root[0])) continue;
        if (combo) {
            wchar_t item[64];
            _snwprintf_s(item, _countof(item), _TRUNCATE, L"%c:  (locked)", root[0]);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
            if (want && towupper(want[0]) == root[0]) sel = n;
        }
        Log(L"bitlocker: %c: is locked", root[0]);
        n++;
    }
    if (combo && n) SendMessageW(combo, CB_SETCURSEL, sel, 0);
    return n;
}

static const wchar_t *ErrorText(DWORD rc)
{
    switch (rc) {
    case 0: return L"Unlocked.";
    case 0x80310027: return L"Wrong password or recovery key.";                       /* FVE_E_FAILED_AUTHENTICATION */
    case 0x80310033: return L"The recovery key has the wrong format (8 groups of 6 digits).";
    case 0x80310000: return L"The drive is still locked.";
    case 0x80070002: case 0x80070003: return L"The key file was not found.";
    case 0x80310005: case 0x8031000D: return L"This key file does not belong to the drive.";
    case 1: return L"BitLocker (WMI) did not answer - see the log.";
    case 2: return L"BitLocker does not know this drive.";
    case 0xFFFFFFFF: return L"PowerShell could not be started.";
    }
    return NULL;
}

/* worker: PowerShell gets "<drive>\n<secret>\n" on stdin; its output goes to logs\bitlocker-unlock.log */
static DWORD WINAPI UnlockThread(LPVOID p)
{
    static const wchar_t script[] =
        L"$ErrorActionPreference='Stop';trap{[Console]::Error.WriteLine($_);exit 1};"
        L"$d=[Console]::In.ReadLine();$k=[Console]::In.ReadLine();"
        L"$v=Get-CimInstance -Namespace root/cimv2/Security/MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume|Where-Object{$_.DriveLetter -eq $d};"
        L"if(-not $v){[Console]::Error.WriteLine('no BitLocker volume '+$d);exit 2};"
        L"if($k.StartsWith('keyfile:')){$e=Invoke-CimMethod -InputObject $v -MethodName GetExternalKeyFromFile -Arguments @{PathWithFileName=$k.Substring(8)};"
        L"if($e.ReturnValue -ne 0){$r=$e}else{$r=Invoke-CimMethod -InputObject $v -MethodName UnlockWithExternalKey -Arguments @{ExternalKey=$e.ExternalKey}}}"
        L"elseif($k -match '^\\d{6}(-\\d{6}){7}$'){$r=Invoke-CimMethod -InputObject $v -MethodName UnlockWithNumericalPassword -Arguments @{NumericalPassword=$k}}"
        L"else{$r=Invoke-CimMethod -InputObject $v -MethodName UnlockWithPassphrase -Arguments @{Passphrase=$k}};"
        L"$k=$null;'ReturnValue '+$r.ReturnValue;exit [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$r.ReturnValue),0)";
    wchar_t ps[MAX_PATH], line[2048], logf[MAX_PATH];
    char in[1100];                 /* drive + up to 255 chars of secret in UTF-8 (3 bytes each) */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, out;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 0xFFFFFFFF, n;
    int len;
    (void)p;

    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", ps, MAX_PATH);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"\"%s\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%s\"", ps, script);
    _snwprintf_s(logf, _countof(logf), _TRUNCATE, L"%s\\bitlocker-unlock.log", WPS_LOGDIR);
    CreateDirectoryW(WPS_LOGDIR, NULL);
    out = CreateFileW(logf, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (CreatePipe(&rd, &wr, &sa, 0)) {
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);   /* only the read end goes to the child */
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = rd;
        si.hStdOutput = si.hStdError = out != INVALID_HANDLE_VALUE ? out : NULL;
        if (CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(rd); rd = NULL;
            len = WideCharToMultiByte(CP_UTF8, 0, B.drive, -1, in, 8, NULL, NULL);
            if (len > 0) { in[len - 1] = '\n'; len += WideCharToMultiByte(CP_UTF8, 0, B.key, -1, in + len, (int)sizeof(in) - len - 2, NULL, NULL); }
            if (len > 1) { in[len - 1] = '\n'; WriteFile(wr, in, (DWORD)len, &n, NULL); }
            SecureZeroMemory(in, sizeof(in));
            CloseHandle(wr); wr = NULL;
            if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1); Log(L"bitlocker: PowerShell timed out"); }
            GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        } else Log(L"bitlocker: PowerShell start failed err=%lu", GetLastError());
    }
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    SecureZeroMemory(B.key, sizeof(B.key));
    PostMessageW(B.wnd, WM_APP_DONE, 0, (LPARAM)rc);
    return 0;
}

/* key information worker: which protectors the drive has (GetKeyProtectors + GetKeyProtectorType), shown in the
   window's info line. PowerShell gets the drive on stdin, prints "P <type> <id>" lines on stdout (a pipe). */
static DWORD WINAPI InfoThread(LPVOID p)
{
    static const wchar_t script[] =
        L"$ErrorActionPreference='Stop';trap{'ERR '+$_;exit 1};$d=[Console]::In.ReadLine();"
        L"$v=Get-CimInstance -Namespace root/cimv2/Security/MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume|Where-Object{$_.DriveLetter -eq $d};"
        L"if(-not $v){'ERR no BitLocker volume';exit 2};"
        L"foreach($i in (Invoke-CimMethod -InputObject $v -MethodName GetKeyProtectors -Arguments @{KeyProtectorType=[uint32]0}).VolumeKeyProtectorID)"
        L"{'P '+(Invoke-CimMethod -InputObject $v -MethodName GetKeyProtectorType -Arguments @{VolumeKeyProtectorID=$i}).KeyProtectorType+' '+$i}";
    LONG gen = (LONG)((INT_PTR)p >> 8);
    char drive[4] = { (char)((INT_PTR)p & 0xFF), ':', '\n', 0 };
    wchar_t ps[MAX_PATH], line[2048], *text;
    char out[8192];
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n, got = 0;
    BOOL pw = FALSE, rk = FALSE, kf = FALSE, tpm = FALSE, other = FALSE;
    wchar_t rkid[64] = L"";
    char *s;

    out[0] = 0;
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", ps, MAX_PATH);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"\"%s\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%s\"", ps, script);
    if (CreatePipe(&inR, &inW, &sa, 0) && CreatePipe(&outR, &outW, &sa, 0)) {
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = inR; si.hStdOutput = si.hStdError = outW;
        if (CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(inR); inR = NULL; CloseHandle(outW); outW = NULL;
            WriteFile(inW, drive, 3, &n, NULL);
            CloseHandle(inW); inW = NULL;
            while (got < sizeof(out) - 1 && ReadFile(outR, out + got, (DWORD)(sizeof(out) - 1 - got), &n, NULL) && n) got += n;
            out[got] = 0;
            WaitForSingleObject(pi.hProcess, 60000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        } else Log(L"bitlocker: key info: PowerShell start failed err=%lu", GetLastError());
    }
    if (inR) CloseHandle(inR);
    if (inW) CloseHandle(inW);
    if (outR) CloseHandle(outR);
    if (outW) CloseHandle(outW);
    /* "P <type> <{id}>" lines; types: 1 TPM, 2 key file, 3 recovery key, 4-6 TPM + PIN / key file, 8 password */
    for (s = out; s && *s; ) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        if (s[0] == 'P' && s[1] == ' ') {
            int t = atoi(s + 2);
            char *id = strchr(s + 2, ' ');
            if (t == 8) pw = TRUE;
            else if (t == 2) kf = TRUE;
            else if (t == 3) {
                rk = TRUE;
                if (id && !rkid[0]) {
                    int k = 0;
                    for (id++; *id && *id != '\r' && k < 40; id++) if (*id != '{' && *id != '}') rkid[k++] = (wchar_t)*id;
                    rkid[k] = 0;
                }
            } else if (t == 1 || (t >= 4 && t <= 6)) tpm = TRUE;
            else if (t) other = TRUE;
        } else if (!strncmp(s, "ERR", 3)) Log(L"bitlocker: key info: %S", s);
        s = nl ? nl + 1 : NULL;
    }
    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 512 * sizeof(wchar_t));
    if (text) {
        if (!pw && !rk && !kf && !tpm && !other)
            wcscpy_s(text, 512, L"No key information (see the log).");
        else {
            /* one fact per line: the ID did not fit next to the list (VM 2026-11-28) */
            _snwprintf_s(text, 512, _TRUNCATE, L"Unlocks with:%s%s%s%s%s%s%s",
                         rk ? L" recovery key" : L"",
                         pw ? (rk ? L", password" : L" password") : L"",
                         kf ? ((rk || pw) ? L", key file" : L" key file") : L"",
                         rk && rkid[0] ? L"\nRecovery key ID: " : L"", rk && rkid[0] ? rkid : L"",
                         tpm ? L"\nTPM: only in that PC's Windows - use the recovery key." : L"",
                         (!pw && !rk && !kf) ? L"\nNo password, recovery key or key file: cannot be unlocked here." : L"");
        }
        Log(L"bitlocker: key info %c: %s", (wchar_t)drive[0], text);
    }
    if (!text || !PostMessageW(B.wnd, WM_APP_INFO, (WPARAM)gen, (LPARAM)text)) { if (text) HeapFree(GetProcessHeap(), 0, text); }
    return 0;
}

/* key information for the selected drive (background; the newest request wins) */
static void StartInfo(void)
{
    wchar_t sel[64] = L"";
    int i = (int)SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETCURSEL, 0, 0);
    LONG gen;
    HANDLE t;
    if (i < 0) { SetDlgItemTextW(B.wnd, IDC_B_INFO, L""); return; }
    SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETLBTEXT, i, (LPARAM)sel);
    gen = InterlockedIncrement(&B.infoGen);
    SetDlgItemTextW(B.wnd, IDC_B_INFO, L"Reading the key information...");
    t = CreateThread(NULL, 0, InfoThread, (LPVOID)(INT_PTR)(((INT_PTR)gen << 8) | (sel[0] & 0xFF)), 0, NULL);
    if (t) CloseHandle(t);
}

static void SetBusy(BOOL busy)
{
    B.busy = busy;
    EnableWindow(GetDlgItem(B.wnd, IDC_B_UNLOCK), !busy);
    EnableWindow(GetDlgItem(B.wnd, IDC_B_KEY), !busy);
    EnableWindow(GetDlgItem(B.wnd, IDC_B_DRIVE), !busy);
    EnableWindow(GetDlgItem(B.wnd, IDC_B_FILE), !busy);
}

static void StartUnlock(void)
{
    wchar_t sel[64] = L"", k[300], *s, *d;
    int i = (int)SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETCURSEL, 0, 0), digits = 0;
    HANDLE t;
    if (B.busy || i < 0) return;
    SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETLBTEXT, i, (LPARAM)sel);
    GetDlgItemTextW(B.wnd, IDC_B_KEY, k, _countof(k));
    if (!k[0]) { SetDlgItemTextW(B.wnd, IDC_B_STATUS, L"Type the password or the recovery key."); return; }
    /* recovery key typed without dashes / with spaces: 48 digits -> 8 groups of 6 */
    for (s = k; *s; s++) if (iswdigit(*s)) digits++; else if (*s != L'-' && *s != L' ') { digits = -1; break; }
    if (digits == 48) {
        for (s = k, d = B.key, i = 0; *s; s++) {
            if (!iswdigit(*s)) continue;
            if (i && i % 6 == 0) *d++ = L'-';
            *d++ = *s; i++;
        }
        *d = 0;
    } else wcscpy_s(B.key, _countof(B.key), k);
    SecureZeroMemory(k, sizeof(k));
    B.drive[0] = sel[0]; B.drive[1] = L':'; B.drive[2] = 0;
    SetDlgItemTextW(B.wnd, IDC_B_STATUS, L"Unlocking...");
    SetBusy(TRUE);
    Log(L"bitlocker: unlocking %s (%s)", B.drive, digits == 48 ? L"recovery key" : L"password");
    t = CreateThread(NULL, 0, UnlockThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else { SetBusy(FALSE); SecureZeroMemory(B.key, sizeof(B.key)); SetDlgItemTextW(B.wnd, IDC_B_STATUS, L"Could not start."); }
}

/* "Key file...": a BitLocker startup key (.BEK, usually on a stick) -> GetExternalKeyFromFile + UnlockWithExternalKey */
static void StartUnlockFile(void)
{
    wchar_t sel[64] = L"", path[MAX_PATH] = L"";
    OPENFILENAMEW of;
    int i = (int)SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETCURSEL, 0, 0);
    HANDLE t;
    if (B.busy || i < 0) return;
    ZeroMemory(&of, sizeof(of));
    of.lStructSize = sizeof(of);
    of.hwndOwner = B.wnd;
    of.lpstrFilter = L"BitLocker key file (*.bek)\0*.bek\0All files\0*.*\0";
    of.lpstrFile = path;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"BitLocker key file";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    SendMessageW(GetDlgItem(B.wnd, IDC_B_DRIVE), CB_GETLBTEXT, i, (LPARAM)sel);
    _snwprintf_s(B.key, _countof(B.key), _TRUNCATE, L"keyfile:%s", path);
    B.drive[0] = sel[0]; B.drive[1] = L':'; B.drive[2] = 0;
    SetDlgItemTextW(B.wnd, IDC_B_STATUS, L"Unlocking with the key file...");
    SetBusy(TRUE);
    Log(L"bitlocker: unlocking %s (key file %s)", B.drive, path);
    t = CreateThread(NULL, 0, UnlockThread, NULL, 0, NULL);
    if (t) CloseHandle(t); else { SetBusy(FALSE); SecureZeroMemory(B.key, sizeof(B.key)); SetDlgItemTextW(B.wnd, IDC_B_STATUS, L"Could not start."); }
}

static void Layout(void)
{
    int m = BS(14), w = BS(420), x2 = BS(150), y = m, ew = w - x2 - m;
    RECT r = { 0, 0, w, 0 };
    HWND h = B.wnd;
    HFONT old = B.font;
    B.s = WindowDpi(h) / 96.0;
    m = BS(14); w = BS(420); x2 = BS(150); y = m; ew = w - x2 - m;
    B.font = CreateFontW(-BS(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    MoveWindow(GetDlgItem(h, IDC_B_L1), m, y + BS(4), x2 - m, BS(20), FALSE);
    MoveWindow(GetDlgItem(h, IDC_B_DRIVE), x2, y, ew, BS(200), FALSE); y += BS(32);
    MoveWindow(GetDlgItem(h, IDC_B_INFO), m, y, w - 2 * m, BS(58), FALSE); y += BS(64);   /* key information: 3 lines, full width */
    MoveWindow(GetDlgItem(h, IDC_B_L2), m, y + BS(4), x2 - m, BS(36), FALSE);
    MoveWindow(GetDlgItem(h, IDC_B_KEY), x2, y, ew, BS(26), FALSE); y += BS(32);
    MoveWindow(GetDlgItem(h, IDC_B_SHOW), x2, y, ew, BS(22), FALSE); y += BS(30);
    MoveWindow(GetDlgItem(h, IDC_B_STATUS), m, y, w - 2 * m, BS(40), FALSE); y += BS(46);
    MoveWindow(GetDlgItem(h, IDC_B_FILE), m, y, BS(110), BS(30), FALSE);
    MoveWindow(GetDlgItem(h, IDC_B_UNLOCK), w - m - BS(200), y, BS(96), BS(30), FALSE);
    MoveWindow(GetDlgItem(h, IDC_B_CLOSE), w - m - BS(96), y, BS(96), BS(30), FALSE); y += BS(30) + m;
    {   int id;
        for (id = IDC_B_DRIVE; id <= IDC_B_FILE; id++) SendMessageW(GetDlgItem(h, id), WM_SETFONT, (WPARAM)B.font, FALSE); }
    if (old) DeleteObject(old);
    r.right = w;        /* w is only known after the DPI (it was 0 here: a 22 px wide window, VM 2026-11-28) */
    r.bottom = y;
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(h, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(h, GWL_EXSTYLE));
    SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(h, NULL, TRUE);
}

static LRESULT CALLBACK BdeProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: case IDC_B_UNLOCK: StartUnlock(); return 0;
        case IDC_B_FILE: StartUnlockFile(); return 0;
        case IDC_B_DRIVE: if (HIWORD(wp) == CBN_SELCHANGE) StartInfo(); return 0;
        case IDCANCEL: case IDC_B_CLOSE: if (!B.busy) DestroyWindow(h); return 0;
        case IDC_B_SHOW: {
            BOOL show = IsDlgButtonChecked(h, IDC_B_SHOW) == BST_CHECKED;
            SendDlgItemMessageW(h, IDC_B_KEY, EM_SETPASSWORDCHAR, show ? 0 : 0x25CF, 0);
            InvalidateRect(GetDlgItem(h, IDC_B_KEY), NULL, TRUE);
            return 0; }
        }
        break;
    case WM_APP_DONE: {
        DWORD rc = (DWORD)lp;
        const wchar_t *t = ErrorText(rc);
        wchar_t msg[200];
        SetBusy(FALSE);
        if (t) wcscpy_s(msg, _countof(msg), t);
        else _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"BitLocker error 0x%08lX.", rc);
        Log(L"bitlocker: %s -> 0x%08lX %s", B.drive, rc, msg);
        if (rc == 0) {
            SetDlgItemTextW(h, IDC_B_KEY, L"");
            _snwprintf_s(msg, _countof(msg), _TRUNCATE, L"%s unlocked.%s", B.drive,
                         ListLocked(GetDlgItem(h, IDC_B_DRIVE), NULL) ? L"" : L" No locked drive left.");
            EnableWindow(GetDlgItem(h, IDC_B_UNLOCK), SendDlgItemMessageW(h, IDC_B_DRIVE, CB_GETCOUNT, 0, 0) > 0);
            EnableWindow(GetDlgItem(h, IDC_B_FILE), SendDlgItemMessageW(h, IDC_B_DRIVE, CB_GETCOUNT, 0, 0) > 0);
            StartInfo();    /* the next locked drive, or empty */
        } else SetFocus(GetDlgItem(h, IDC_B_KEY));
        SetDlgItemTextW(h, IDC_B_STATUS, msg);
        return 0; }
    case WM_APP_INFO: {     /* key information from InfoThread; an older request's answer is dropped */
        wchar_t *text = (wchar_t *)lp;
        if ((LONG)wp == B.infoGen) SetDlgItemTextW(h, IDC_B_INFO, text);
        HeapFree(GetProcessHeap(), 0, text);
        return 0; }
    case WM_ACTIVATE:
        /* opened at boot before the shell: the focus was lost when the desktop came up and typed text went to the
           drive list (VM 2026-11-28); on activation the cursor goes to an empty password field */
        if (LOWORD(wp) != WA_INACTIVE && !B.busy && GetWindowTextLengthW(GetDlgItem(h, IDC_B_KEY)) == 0) {
            SetFocus(GetDlgItem(h, IDC_B_KEY));
            return 0;
        }
        break;
    case WM_DPICHANGED:
        Layout();
        return 0;
    case WM_CLOSE:
        if (!B.busy) DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        SecureZeroMemory(B.key, sizeof(B.key));
        if (B.font) DeleteObject(B.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int BitLockerCommand(int argc, wchar_t **argv)
{
    const wchar_t *want = NULL;
    BOOL check = FALSE;
    WNDCLASSW wc;
    HWND h;
    MSG msg;
    int i, n;
    HINSTANCE hi = GetModuleHandleW(NULL);

    for (i = 2; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/check")) check = TRUE;
        else if (iswalpha(argv[i][0]) && argv[i][1] == L':') want = argv[i];
    }
    if ((h = FindWindowW(BDE_CLASS, NULL)) != NULL) { ForceForeground(h); return 0; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    n = ListLocked(NULL, NULL);
    Log(L"bitlocker: %d locked drive(s)%s", n, check ? L" (/check)" : L"");
    if (check && n == 0) { CoUninitialize(); return 0; }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = BdeProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = BDE_CLASS;
    wc.hIcon = LoadIconW(NULL, IDI_SHIELD);
    RegisterClassW(&wc);
    B.wnd = h = CreateWindowExW(WS_EX_DLGMODALFRAME, BDE_CLASS, L"Unlock BitLocker drive", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 400, 240, NULL, NULL, hi, NULL);
    if (!h) { CoUninitialize(); return 1; }
#define MK(cls, text, style, id) CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), 0, 0, 10, 10, h, (HMENU)(INT_PTR)(id), hi, NULL)
    MK(L"STATIC", L"Drive:", SS_LEFT, IDC_B_L1);
    MK(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_B_DRIVE);
    MK(L"STATIC", L"Password or recovery key:", SS_LEFT, IDC_B_L2);
    MK(L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | WS_TABSTOP, IDC_B_KEY);
    MK(L"BUTTON", L"Show characters", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_B_SHOW);
    MK(L"STATIC", n ? L"" : L"No locked BitLocker drive found.", SS_LEFT, IDC_B_STATUS);
    MK(L"BUTTON", L"Unlock", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_B_UNLOCK);
    MK(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_B_CLOSE);
    MK(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_B_INFO);
    MK(L"BUTTON", L"Key file...", BS_PUSHBUTTON | WS_TABSTOP, IDC_B_FILE);
#undef MK
    SendDlgItemMessageW(h, IDC_B_KEY, EM_SETPASSWORDCHAR, 0x25CF, 0);
    SendDlgItemMessageW(h, IDC_B_KEY, EM_LIMITTEXT, 255, 0);
    ListLocked(GetDlgItem(h, IDC_B_DRIVE), want);
    EnableWindow(GetDlgItem(h, IDC_B_UNLOCK), n > 0);
    EnableWindow(GetDlgItem(h, IDC_B_FILE), n > 0);
    Layout();
    if (n > 0) StartInfo();
    {   /* centered on the monitor under the mouse */
        POINT pt; MONITORINFO mi = { sizeof(mi) }; RECT r;
        GetCursorPos(&pt);
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
        GetWindowRect(h, &r);
        SetWindowPos(h, HWND_TOP, (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2, 0, 0, SWP_NOSIZE);
    }
    /* twice: started as a hidden start-up step (WinPE-ShellStart: STARTF_USESHOWWINDOW + SW_HIDE), the FIRST
       ShowWindow of the process takes SW_HIDE from STARTUPINFO instead (VM 2026-11-28: window created, visible=0) */
    ShowWindow(h, SW_SHOWNORMAL);
    ShowWindow(h, SW_SHOWNORMAL);
    ForceForeground(h);
    SetFocus(GetDlgItem(h, IDC_B_KEY));
    {   RECT r; GetWindowRect(h, &r);
        Log(L"bitlocker: window %d,%d-%d,%d visible=%d dpi=%u", r.left, r.top, r.right, r.bottom, IsWindowVisible(h), WindowDpi(h)); }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(h, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
