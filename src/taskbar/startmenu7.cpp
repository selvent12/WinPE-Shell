/*
 * WinPE-Shell: Windows 7 style two-column Start menu (see startmenu7.h).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <precomp.h>

#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>

#include "startmenu7.h"
#include "win11style.h"
#include "../jconfig/jcfg.h"

#define SM7_CLASS   TEXT("WinPEShellStartMenu7")
#define IDC_SM7_SEARCH  0x7E01
#define ID_SM7_RESTART  0x7E10
#define ID_SM7_RESTARTSHELL 0x7E11
#define ID_SM7_SHUTDOWN 0x7E12

namespace {

// ---------------------------------------------------------------- data

struct Node {
    std::wstring name;                  // display name (file name without extension)
    std::vector<std::wstring> paths;    // file: one path; folder: same-named folders of all roots (merged)
    bool folder = false;
    bool expanded = false;
    int icon = -1;                      // system image list index, -1 = not looked up yet
    std::vector<Node> children;
};

enum RowKind { R_ITEM, R_SEP, R_EMPTY };
struct Row { RowKind kind; Node *node; int depth; };

struct RightItem { std::wstring text, command, parameters; bool sep; };

enum Mode { M_HOME, M_ALL, M_SEARCH };
enum HotArea { H_NONE, H_LEFT, H_FOOTER, H_RIGHT, H_SHUT, H_ARROW };

static bool IsDots(const wchar_t *n) { return n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2])); }

// extra folders (JS_STARTMENU.extra_folders) hold whole portable programs: only launchable files are listed there
static bool IsLaunchable(const wchar_t *file)
{
    const wchar_t *ext = PathFindExtensionW(file);
    static const wchar_t *ok[] = {L".lnk", L".exe", L".url", L".msc", L".bat", L".cmd"};
    for (auto e : ok)
        if (!_wcsicmp(ext, e))
            return true;
    return false;
}

static void ScanDir(const std::wstring &dir, Node &parent, bool launchableOnly = false, int depth = 0)
{
    if (depth > 8)
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (IsDots(fd.cFileName) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;                   // no junction loops
        std::wstring name = fd.cFileName, full = dir + L"\\" + fd.cFileName;
        if (!isDir) {
            if (!_wcsicmp(name.c_str(), L"desktop.ini") || (launchableOnly && !IsLaunchable(fd.cFileName)))
                continue;
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos && dot > 0)
                name.erase(dot);
        }
        Node *same = NULL;
        for (size_t i = 0; i < parent.children.size(); i++)
            if (parent.children[i].folder == isDir && !_wcsicmp(parent.children[i].name.c_str(), name.c_str())) {
                same = &parent.children[i];
                break;
            }
        if (same) {                     // all users + user Start menu: merge folders, first shortcut wins
            if (isDir) {
                same->paths.push_back(full);
                ScanDir(full, *same, launchableOnly, depth + 1);
            }
            continue;
        }
        Node n;
        n.name = name;
        n.paths.push_back(full);
        n.folder = isDir;
        parent.children.push_back(n);
        if (isDir) {
            ScanDir(full, parent.children.back(), launchableOnly, depth + 1);
            if (launchableOnly && parent.children.back().children.empty())
                parent.children.pop_back();     // e.g. a "data" sub-folder of a portable program
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// merges src into dst like ScanDir merges two roots (folders by name; existing shortcuts win)
static void MergeTree(Node &dst, const Node &src)
{
    for (const Node &s : src.children) {
        Node *same = NULL;
        for (auto &d : dst.children)
            if (d.folder == s.folder && !_wcsicmp(d.name.c_str(), s.name.c_str())) {
                same = &d;
                break;
            }
        if (!same)
            dst.children.push_back(s);
        else if (s.folder) {
            same->paths.insert(same->paths.end(), s.paths.begin(), s.paths.end());
            MergeTree(*same, s);
        }
    }
}

// ---------------------------------------------------------------- extra folders (background scan)
// JS_STARTMENU.extra_folders: ["*:\\Programs", "%SystemDrive%\\Tools", ...]. "*:\" = that folder on every fixed or
// removable drive (CD/DVD and network drives are skipped: slow or not ready). Read at run time, so programs copied
// there later (e.g. onto the boot stick) show up without changing the image. Scanned on a worker thread: a drive
// waking up from standby must not block the menu; the result is merged when it arrives.

#define PM_SM7_EXTRA_DONE   (WM_APP + 0x71)

static CRITICAL_SECTION s_extraLock;
// last complete result (guarded by s_extraLock). Allocated once and never freed: the scan thread may still be
// running when the shell exits ("Restart shell"); a static object would be destroyed under it at process exit.
static Node *s_extraTree = NULL;
static LONG s_extraBusy = 0;
static HWND s_extraNotify = NULL;

static std::vector<std::wstring> ExtraFolderPatterns()
{
    std::vector<std::wstring> v;
    Value arr = JCFG2("JS_STARTMENU", "extra_folders");
    if (arr.GetType() != ArrayVal)
        return v;
    for (size_t i = 0; i < arr.size(); i++) {
        std::wstring p = arr[i].ToString(std::wstring());
        if (p.empty())
            continue;
        wchar_t buf[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(p.c_str(), buf, MAX_PATH * 2) && wcslen(buf) < MAX_PATH)
            p = buf;
        while (p.size() > 3 && p.back() == L'\\')
            p.pop_back();
        v.push_back(p);
    }
    return v;
}

static DWORD WINAPI ExtraScanThread(LPVOID)
{
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    Node tree;
    tree.folder = true;
    for (const std::wstring &pat : ExtraFolderPatterns()) {
        std::vector<std::wstring> dirs;
        if (pat.size() >= 3 && pat[0] == L'*' && pat[1] == L':' && pat[2] == L'\\') {
            DWORD drives = GetLogicalDrives();
            for (int d = 2; d < 26; d++) {          // C: .. Z:
                if (!(drives & (1u << d)))
                    continue;
                wchar_t root[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
                UINT t = GetDriveTypeW(root);
                if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK)
                    dirs.push_back(std::wstring(root, 2) + pat.substr(2));
            }
        } else
            dirs.push_back(pat);
        for (const std::wstring &d : dirs) {
            DWORD a = GetFileAttributesW(d.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
                ScanDir(d, tree, true);
        }
    }
    SetErrorMode(oldMode);
    EnterCriticalSection(&s_extraLock);
    *s_extraTree = tree;
    LeaveCriticalSection(&s_extraLock);
    InterlockedExchange(&s_extraBusy, 0);
    if (s_extraNotify)
        PostMessage(s_extraNotify, PM_SM7_EXTRA_DONE, 0, 0);
    return 0;
}

static void EnsureExtraInit()
{
    static bool init = false;
    if (!init) {
        InitializeCriticalSection(&s_extraLock);
        s_extraTree = new Node();
        s_extraTree->folder = true;
        init = true;
    }
}

static void StartExtraScan()
{
    EnsureExtraInit();
    if (ExtraFolderPatterns().empty() || InterlockedCompareExchange(&s_extraBusy, 1, 0) != 0)
        return;
    HANDLE t = CreateThread(NULL, 0, ExtraScanThread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange(&s_extraBusy, 0);
}

static void MergeExtra(Node &root)
{
    EnsureExtraInit();
    EnterCriticalSection(&s_extraLock);
    MergeTree(root, *s_extraTree);
    LeaveCriticalSection(&s_extraLock);
}

static void SortTree(Node &n)
{
    // Windows 7 order: shortcuts first, then folders, each alphabetical
    std::sort(n.children.begin(), n.children.end(), [](const Node &a, const Node &b) {
        if (a.folder != b.folder)
            return !a.folder;
        return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (auto &c : n.children)
        SortTree(c);
}

static Node *FindPath(Node &root, const std::wstring &rel)   // "Tools\Notepad2" (display names)
{
    Node *cur = &root;
    size_t start = 0;
    while (cur) {
        size_t bs = rel.find(L'\\', start);
        std::wstring part = rel.substr(start, bs == std::wstring::npos ? std::wstring::npos : bs - start);
        Node *next = NULL;
        for (auto &c : cur->children)
            if (!_wcsicmp(c.name.c_str(), part.c_str()) && (bs != std::wstring::npos ? c.folder : true)) {
                next = &c;
                break;
            }
        if (!next || bs == std::wstring::npos)
            return next;
        cur = next;
        start = bs + 1;
    }
    return NULL;
}

static void CollectMatches(Node &n, const std::wstring &q, std::vector<Node *> &out)
{
    for (auto &c : n.children) {
        if (out.size() >= 60)
            return;
        if (c.folder)
            CollectMatches(c, q, out);
        else if (StrStrIW(c.name.c_str(), q.c_str()))
            out.push_back(&c);
    }
}

static std::wstring JStr(const Value &v, const TCHAR *key)
{
    if (v.GetType() != ObjectVal || !v.HasKey(key))
        return L"";
    return v[key].ToString(std::wstring());
}

// ---------------------------------------------------------------- window

class StartMenu7 {
public:
    static StartMenu7 &Get() { static StartMenu7 s; return s; }

    void Toggle(HWND hwndStartButton);
    void Hide();
    bool Visible() const { return _hwnd && IsWindowVisible(_hwnd); }

private:
    HWND _hwnd = NULL, _edit = NULL, _startBtn = NULL;
    HFONT _font = NULL, _fontBold = NULL;
    HIMAGELIST _ilSmall = NULL, _ilLarge = NULL;
    int _cxSmall = 16, _cxLarge = 32;
    DWORD _hiddenAt = 0;

    Node _root;
    std::vector<Node *> _pinned, _matches;
    std::vector<Row> _rows;
    std::vector<RightItem> _right;
    std::wstring _title;
    Mode _mode = M_HOME;
    int _sel = -1;              // keyboard/mouse selection in _rows (or _rows.size() = footer)
    HotArea _hot = H_NONE;
    int _hotIdx = -1;
    int _scroll = 0;            // first visible pixel of the left list
    bool _tracking = false;

    // layout (client coordinates)
    RECT _rcLeft{}, _rcList{}, _rcFooter{}, _rcSearch{}, _rcRight{}, _rcShut{}, _rcArrow{};
    int _rowSmall = 0, _rowLarge = 0, _rowSep = 0, _rowRight = 0;

    COLORREF _clrLeft = RGB(255, 255, 255), _clrLeftText = RGB(20, 20, 20), _clrHot = RGB(204, 232, 255),
             _clrHotBorder = RGB(153, 209, 255), _clrRight = RGB(30, 34, 48), _clrRightText = RGB(240, 240, 240),
             _clrRightHot = RGB(62, 70, 92), _clrBorder = RGB(80, 86, 100), _clrSep = RGB(225, 225, 225),
             _clrGray = RGB(110, 110, 110), _clrSearch = RGB(255, 255, 255), _clrShut = RGB(48, 54, 70);

    // JS_STARTMENU.style = "win11": same menu, Windows 11 look (search box on top, rounded corners and highlights,
    // colors of JS_TASKBAR.theme, floating 8 px above the taskbar, centered on the screen with JS_TASKBAR.align center)
    bool _w11 = false;
    HBRUSH _brSearch = NULL;
    void SetColors();
    void DrawSearch(HDC hdc);
    void FillItem(HDC hdc, const RECT &rc, COLORREF c, COLORREF frame);

    void Create();
    void LoadConfig();
    void Rescan();
    void Layout();
    void BuildRows();
    void AddTreeRows(Node &n, int depth);
    int RowHeight(const Row &r) const;
    int ContentHeight() const;
    void ClampScroll();
    void EnsureVisible(int idx);
    int RowAt(POINT pt) const;
    int RightAt(POINT pt) const;
    int IconFor(Node *n);

    void Paint(HDC hdc);
    void DrawLeft(HDC hdc);
    void DrawRight(HDC hdc);

    void Activate(int idx);
    void ActivateRight(int idx);
    void RunCommand(const std::wstring &cmd, const std::wstring &params);
    void SetMode(Mode m);
    void OnSearchChanged();
    bool OnKey(UINT vk);
    void ShowShutdownMenu();
    void SetHot(HotArea a, int idx);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp);
};

void StartMenu7::SetColors()
{
    _w11 = _wcsicmp(JCFG2_DEF("JS_STARTMENU", "style", TEXT("classic")).ToString(std::wstring(L"classic")).c_str(), L"win11") == 0;
    if (!_w11)
        return;                         // the Windows 7 colors above
    if (W11_Dark()) {
        _clrLeft = RGB(40, 40, 40);  _clrRight = RGB(33, 33, 33);  _clrLeftText = _clrRightText = RGB(255, 255, 255);
        _clrHot = RGB(58, 58, 58);   _clrHotBorder = _clrHot;      _clrRightHot = RGB(52, 52, 52);
        _clrBorder = RGB(72, 72, 72); _clrSep = RGB(62, 62, 62);   _clrGray = RGB(165, 165, 165);
        _clrSearch = RGB(29, 29, 29); _clrShut = RGB(45, 45, 45);
    } else {
        _clrLeft = RGB(249, 249, 249); _clrRight = RGB(238, 238, 238); _clrLeftText = _clrRightText = RGB(26, 26, 26);
        _clrHot = RGB(232, 232, 232);  _clrHotBorder = _clrHot;        _clrRightHot = RGB(225, 225, 225);
        _clrBorder = RGB(200, 200, 200); _clrSep = RGB(222, 222, 222); _clrGray = RGB(110, 110, 110);
        _clrSearch = RGB(255, 255, 255); _clrShut = RGB(230, 230, 230);
    }
    if (_brSearch)
        DeleteObject(_brSearch);
    _brSearch = CreateSolidBrush(_clrSearch);
}

void StartMenu7::LoadConfig()
{
    SetColors();
    Value cfg = JCFG2("JS_STARTMENU", "win7");
    _title = JStr(cfg, TEXT("title"));

    _right.clear();
    bool norun = JCFG2_DEF("JS_STARTMENU", "norun", false).ToBool();
    if (cfg.GetType() == ObjectVal && cfg.HasKey(TEXT("right")) && cfg[TEXT("right")].GetType() == ArrayVal) {
        const Value &arr = cfg[TEXT("right")];
        for (size_t i = 0; i < arr.size(); i++) {
            const Value &it = arr[i];
            RightItem r;
            r.text = JStr(it, TEXT("text"));
            r.command = JStr(it, TEXT("command"));
            r.parameters = JStr(it, TEXT("parameters"));
            r.sep = r.text.empty() && r.command.empty();
            if (!r.sep && r.text.empty())
                r.text = r.command;
            if (norun && r.command == L"::run")
                continue;
            _right.push_back(r);
        }
    } else {
        _right.push_back({L"This PC", L"::explorer", L"", false});
        _right.push_back({L"Command Prompt", L"cmd.exe", L"", false});
        _right.push_back({L"", L"", L"", true});
        _right.push_back({L"Display settings", L"ms-settings:display", L"", false});
        _right.push_back({L"Task Manager", L"taskmgr.exe", L"", false});
        if (!norun) {
            _right.push_back({L"", L"", L"", true});
            _right.push_back({L"Run...", L"::run", L"", false});
        }
    }
}

void StartMenu7::Rescan()
{
    _root = Node();
    _root.folder = true;
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, path)))
        ScanDir(path, _root);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, path)))
        ScanDir(path, _root);
    MergeExtra(_root);                  // extra_folders: last complete background scan
    SortTree(_root);

    _pinned.clear();
    Value cfg = JCFG2("JS_STARTMENU", "win7");
    if (cfg.GetType() == ObjectVal && cfg.HasKey(TEXT("pinned")) && cfg[TEXT("pinned")].GetType() == ArrayVal) {
        const Value &arr = cfg[TEXT("pinned")];
        for (size_t i = 0; i < arr.size(); i++) {
            Node *n = FindPath(_root, arr[i].ToString(std::wstring()));
            if (n && !n->folder)
                _pinned.push_back(n);
        }
    } else {
        for (auto &c : _root.children)
            if (!c.folder)
                _pinned.push_back(&c);
    }
}

void StartMenu7::Create()
{
    WNDCLASSEX wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_Globals._hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = SM7_CLASS;
    wc.style = CS_DROPSHADOW;
    RegisterClassEx(&wc);

    _hwnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, SM7_CLASS, TEXT("Start Menu"), WS_POPUP | WS_CLIPCHILDREN,
                           0, 0, 10, 10, NULL, NULL, g_Globals._hInstance, NULL);

    NONCLIENTMETRICS ncm = { sizeof(ncm) };
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONT lf = ncm.lfMenuFont;
    _font = CreateFontIndirect(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = lf.lfHeight * 5 / 4;
    _fontBold = CreateFontIndirect(&lf);

    // system image lists without touching a disk: a PC with a blank disk has no C: in WinPE (no icons at all before)
    SHFILEINFOW sfi;
    _ilSmall = (HIMAGELIST)SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    _ilLarge = (HIMAGELIST)SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    int cy;
    if (_ilSmall) ImageList_GetIconSize(_ilSmall, &_cxSmall, &cy);
    if (_ilLarge) ImageList_GetIconSize(_ilLarge, &_cxLarge, &cy);

    _edit = CreateWindowEx(0, WC_EDIT, NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, _hwnd,
                           (HMENU)(INT_PTR)IDC_SM7_SEARCH, g_Globals._hInstance, NULL);
    SendMessage(_edit, WM_SETFONT, (WPARAM)_font, FALSE);
    SendMessage(_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search programs");
    SetWindowSubclass(_edit, EditProc, 1, (DWORD_PTR)this);
}

void StartMenu7::Layout()
{
    int wl = JCFG3_DEF("JS_STARTMENU", "win7", "width_left", 300).ToInt(300);
    int wr = JCFG3_DEF("JS_STARTMENU", "win7", "width_right", 200).ToInt(200);
    int hh = JCFG3_DEF("JS_STARTMENU", "win7", "height", 520).ToInt(520);
    wl = DPI_SX(wl); wr = DPI_SX(wr); hh = DPI_SY(hh);

    RECT rcBtn, rcBar;
    GetWindowRect(_startBtn, &rcBtn);
    GetWindowRect(g_Globals._hwndDesktopBar, &rcBar);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(MonitorFromRect(&rcBtn, MONITOR_DEFAULTTONEAREST), &mi);
    int gap = _w11 ? DPI_SY(8) : 0;     // win11: the menu floats above the taskbar
    hh = min(hh, (int)(rcBar.top - mi.rcMonitor.top) - DPI_SY(8) - gap * 2);
    int x = max((int)mi.rcMonitor.left, (int)rcBtn.left);
    if (_w11) {
        if (W11_Center())
            x = (mi.rcMonitor.left + mi.rcMonitor.right - (wl + wr)) / 2;
        x = max((int)mi.rcMonitor.left + gap, min(x, (int)mi.rcMonitor.right - gap - (wl + wr)));
    }
    SetWindowPos(_hwnd, HWND_TOPMOST, x, rcBar.top - gap - hh, wl + wr, hh, SWP_NOACTIVATE);
    // rounded corners: a window region works without DWM (WinPE); NULL restores the rectangle
    SetWindowRgn(_hwnd, _w11 ? CreateRoundRectRgn(0, 0, wl + wr + 1, hh + 1, DPI_SX(16), DPI_SX(16)) : NULL, TRUE);

    int pad = DPI_SX(6);
    _rowSmall = max(DPI_SY(26), _cxSmall + DPI_SY(8));
    _rowLarge = max(DPI_SY(40), _cxLarge + DPI_SY(8));
    _rowSep = DPI_SY(9);
    _rowRight = DPI_SY(32);

    if (_w11) {
        // search across the whole menu at the top; both columns below it
        int searchH = DPI_SY(34);
        SetRect(&_rcSearch, pad * 3, pad * 3, wl + wr - pad * 3, pad * 3 + searchH);
        int top = _rcSearch.bottom + pad * 2;
        SetRect(&_rcLeft, 0, 0, wl, hh);
        SetRect(&_rcRight, wl, top, wl + wr, hh);
        SetRect(&_rcFooter, pad, hh - pad * 2 - _rowSmall, wl - pad, hh - pad * 2);
        SetRect(&_rcList, pad, top, wl - pad, _rcFooter.top - pad * 2);
    } else {
        SetRect(&_rcLeft, 0, 0, wl, hh);
        SetRect(&_rcRight, wl, 0, wl + wr, hh);
        int searchH = DPI_SY(26);
        SetRect(&_rcSearch, pad * 2, hh - pad * 2 - searchH, wl - pad * 2, hh - pad * 2);
        SetRect(&_rcFooter, pad, _rcSearch.top - pad * 2 - _rowSmall, wl - pad, _rcSearch.top - pad * 2);
        SetRect(&_rcList, pad, pad, wl - pad, _rcFooter.top - pad);
    }
    int shutH = DPI_SY(30), arrowW = DPI_SX(26);
    int shutW = DPI_SX(110);
    _rcShut.left = wl + pad * 2; _rcShut.bottom = hh - pad * 2; _rcShut.top = _rcShut.bottom - shutH;
    _rcShut.right = min(_rcShut.left + shutW, (int)(_rcRight.right - pad * 2 - arrowW));
    SetRect(&_rcArrow, _rcShut.right, _rcShut.top, _rcShut.right + arrowW, _rcShut.bottom);

    if (_w11) {
        int eh = DPI_SY(20);
        MoveWindow(_edit, _rcSearch.left + DPI_SX(12), (_rcSearch.top + _rcSearch.bottom - eh) / 2,
                   _rcSearch.right - _rcSearch.left - DPI_SX(24), eh, FALSE);
    } else
        MoveWindow(_edit, _rcSearch.left + DPI_SX(6), _rcSearch.top + DPI_SY(4), _rcSearch.right - _rcSearch.left - DPI_SX(12),
                   _rcSearch.bottom - _rcSearch.top - DPI_SY(8), FALSE);
}

void StartMenu7::AddTreeRows(Node &n, int depth)
{
    for (auto &c : n.children) {
        _rows.push_back({R_ITEM, &c, depth});
        if (c.folder && c.expanded)
            AddTreeRows(c, depth + 1);
    }
}

void StartMenu7::BuildRows()
{
    _rows.clear();
    switch (_mode) {
    case M_HOME:
        for (Node *n : _pinned)
            _rows.push_back({R_ITEM, n, 0});
        if (_pinned.empty())
            _rows.push_back({R_EMPTY, NULL, 0});
        break;
    case M_ALL:
        AddTreeRows(_root, 0);
        if (_rows.empty())
            _rows.push_back({R_EMPTY, NULL, 0});
        break;
    case M_SEARCH:
        for (Node *n : _matches)
            _rows.push_back({R_ITEM, n, 0});
        if (_matches.empty())
            _rows.push_back({R_EMPTY, NULL, 0});
        break;
    }
    ClampScroll();
}

int StartMenu7::RowHeight(const Row &r) const
{
    if (r.kind == R_SEP) return _rowSep;
    return _mode == M_HOME ? _rowLarge : _rowSmall;
}

int StartMenu7::ContentHeight() const
{
    int h = 0;
    for (auto &r : _rows) h += RowHeight(r);
    return h;
}

void StartMenu7::ClampScroll()
{
    int maxScroll = max(0, ContentHeight() - (int)(_rcList.bottom - _rcList.top));
    _scroll = max(0, min(_scroll, maxScroll));
}

void StartMenu7::EnsureVisible(int idx)
{
    if (idx < 0 || idx >= (int)_rows.size()) return;
    int y = 0;
    for (int i = 0; i < idx; i++) y += RowHeight(_rows[i]);
    int h = RowHeight(_rows[idx]), view = _rcList.bottom - _rcList.top;
    if (y < _scroll) _scroll = y;
    else if (y + h > _scroll + view) _scroll = y + h - view;
    ClampScroll();
}

int StartMenu7::RowAt(POINT pt) const
{
    if (!PtInRect(&_rcList, pt)) return -1;
    int y = _rcList.top - _scroll;
    for (int i = 0; i < (int)_rows.size(); i++) {
        int h = RowHeight(_rows[i]);
        if (pt.y >= y && pt.y < y + h)
            return _rows[i].kind == R_ITEM ? i : -1;
        y += h;
    }
    return -1;
}

int StartMenu7::RightAt(POINT pt) const
{
    if (!PtInRect(&_rcRight, pt)) return -1;
    int y = _rcRight.top + DPI_SY(8) + (_title.empty() ? 0 : DPI_SY(44));
    for (int i = 0; i < (int)_right.size(); i++) {
        int h = _right[i].sep ? _rowSep : _rowRight;
        if (pt.y >= y && pt.y < y + h)
            return _right[i].sep ? -1 : i;
        y += h;
    }
    return -1;
}

int StartMenu7::IconFor(Node *n)
{
    if (n->icon < 0) {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(n->paths[0].c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
            n->icon = sfi.iIcon;
        else
            n->icon = 0;
    }
    return n->icon;
}

// ---------------------------------------------------------------- painting

static void FillColor(HDC hdc, const RECT &rc, COLORREF c)
{
    SetBkColor(hdc, c);
    ExtTextOut(hdc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);
}

static void FrameColor(HDC hdc, const RECT &rc, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(hdc, &rc, b);
    DeleteObject(b);
}

// selected / hot item: a rounded highlight (win11) or the Windows 7 box with a frame
void StartMenu7::FillItem(HDC hdc, const RECT &rc, COLORREF c, COLORREF frame)
{
    if (_w11) {
        RECT r = { rc.left + DPI_SX(2), rc.top + DPI_SY(1), rc.right - DPI_SX(2), rc.bottom - DPI_SY(1) };
        W11_FillRound(hdc, r, DPI_SX(4), c);
        return;
    }
    FillColor(hdc, rc, c);
    if (frame != CLR_INVALID)
        FrameColor(hdc, rc, frame);
}

// search box: Windows 7 = white box with a frame at the bottom of the left column; win11 = rounded box across the
// whole menu at the top, accent line at the bottom while it has the focus
void StartMenu7::DrawSearch(HDC hdc)
{
    bool focus = GetFocus() == _edit;
    if (!_w11) {
        FillColor(hdc, _rcSearch, RGB(255, 255, 255));
        FrameColor(hdc, _rcSearch, focus ? RGB(100, 160, 220) : RGB(180, 180, 180));
        return;
    }
    int r = DPI_SX(4);
    W11_FillRound(hdc, _rcSearch, r, _clrSearch);
    RECT fr = { _rcSearch.left, _rcSearch.top, _rcSearch.right + 1, _rcSearch.bottom + 1 };
    W11_FrameRound(hdc, fr, r, _clrBorder);
    if (focus) {
        RECT line = { _rcSearch.left + r, _rcSearch.bottom - DPI_SY(2), _rcSearch.right - r, _rcSearch.bottom };
        FillColor(hdc, line, W11_Color(W11C_ACCENT));
    }
}

void StartMenu7::DrawLeft(HDC hdc)
{
    FillColor(hdc, _rcLeft, _clrLeft);
    SelectObject(hdc, _font);
    SetBkMode(hdc, TRANSPARENT);

    HRGN clip = CreateRectRgnIndirect(&_rcList);
    SelectClipRgn(hdc, clip);
    int y = _rcList.top - _scroll;
    bool large = _mode == M_HOME;
    int ics = large ? _cxLarge : _cxSmall;
    for (int i = 0; i < (int)_rows.size(); i++) {
        const Row &r = _rows[i];
        int h = RowHeight(r);
        RECT rc = {_rcList.left, y, _rcList.right, y + h};
        y += h;
        if (rc.bottom < _rcList.top || rc.top > _rcList.bottom) continue;
        if (r.kind == R_EMPTY) {
            SetTextColor(hdc, _clrGray);
            std::wstring t = _mode == M_SEARCH ? L"No programs found. Enter runs it as a command." : L"(Empty)";
            RECT tr = rc; tr.left += DPI_SX(10);
            DrawTextW(hdc, t.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        if (i == _sel)
            FillItem(hdc, rc, _clrHot, _clrHotBorder);
        int x = rc.left + DPI_SX(6) + r.depth * DPI_SX(16);
        HIMAGELIST il = large ? _ilLarge : _ilSmall;
        if (il)
            ImageList_Draw(il, IconFor(r.node), hdc, x, rc.top + (h - ics) / 2, ILD_TRANSPARENT);
        RECT tr = {x + ics + DPI_SX(8), rc.top, rc.right - DPI_SX(18), rc.bottom};
        SetTextColor(hdc, _clrLeftText);
        DrawTextW(hdc, r.node->name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (r.node->folder) {
            RECT ar = {rc.right - DPI_SX(18), rc.top, rc.right - DPI_SX(4), rc.bottom};
            SetTextColor(hdc, _clrGray);
            DrawTextW(hdc, r.node->expanded ? L"\x25BE" : L"\x25B8", -1, &ar, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
    }
    SelectClipRgn(hdc, NULL);
    DeleteObject(clip);

    // scroll hint
    if (ContentHeight() > _rcList.bottom - _rcList.top) {
        int view = _rcList.bottom - _rcList.top, total = ContentHeight();
        int th = max(DPI_SY(20), view * view / total);
        int ty = _rcList.top + (view - th) * _scroll / max(1, total - view);
        RECT sb = {_rcList.right - DPI_SX(3), ty, _rcList.right, ty + th};
        FillColor(hdc, sb, RGB(190, 190, 190));
    }

    // footer: All Programs / Back
    RECT sep = {_rcFooter.left + DPI_SX(4), _rcFooter.top - DPI_SY(4), _rcFooter.right - DPI_SX(4), _rcFooter.top - DPI_SY(3)};
    FillColor(hdc, sep, _clrSep);
    if (_sel == (int)_rows.size() || _hot == H_FOOTER)
        FillItem(hdc, _rcFooter, _clrHot, _clrHotBorder);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, _clrLeftText);
    RECT fr = _rcFooter; fr.left += DPI_SX(10);
    const wchar_t *ft = _mode == M_HOME ? (_w11 ? L"\x25B8  All apps" : L"\x25B8  All Programs") : L"\x25C2  Back";
    DrawTextW(hdc, ft, -1, &fr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (!_w11)
        DrawSearch(hdc);                // win11: drawn by Paint, across both columns
}

void StartMenu7::DrawRight(HDC hdc)
{
    FillColor(hdc, _rcRight, _clrRight);
    SetBkMode(hdc, TRANSPARENT);
    int y = _rcRight.top + DPI_SY(8);
    int pad = DPI_SX(8);
    if (!_title.empty()) {
        SelectObject(hdc, _fontBold);
        SetTextColor(hdc, _clrRightText);
        RECT tr = {_rcRight.left + pad * 2, y, _rcRight.right - pad, y + DPI_SY(36)};
        DrawTextW(hdc, _title.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += DPI_SY(44);
    }
    SelectObject(hdc, _font);
    for (int i = 0; i < (int)_right.size(); i++) {
        const RightItem &r = _right[i];
        if (r.sep) {
            RECT s = {_rcRight.left + pad * 2, y + _rowSep / 2, _rcRight.right - pad * 2, y + _rowSep / 2 + 1};
            FillColor(hdc, s, _clrBorder);
            y += _rowSep;
            continue;
        }
        RECT rc = {_rcRight.left + pad, y, _rcRight.right - pad, y + _rowRight};
        if (_hot == H_RIGHT && _hotIdx == i)
            FillItem(hdc, rc, _clrRightHot, CLR_INVALID);
        SetTextColor(hdc, _clrRightText);
        RECT tr = rc; tr.left += pad;
        DrawTextW(hdc, r.text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += _rowRight;
    }

    // Shut down + arrow
    bool noshut = JCFG2_DEF("JS_STARTMENU", "noshutdown", false).ToBool();
    auto button = [&](const RECT &r, bool hot) {
        COLORREF c = hot ? _clrRightHot : _clrShut;
        if (_w11) {
            W11_FillRound(hdc, r, DPI_SX(4), c);
            RECT fr = { r.left, r.top, r.right + 1, r.bottom + 1 };
            W11_FrameRound(hdc, fr, DPI_SX(4), _clrBorder);
        } else {
            FillColor(hdc, r, c);
            FrameColor(hdc, r, _clrBorder);
        }
    };
    if (!noshut) {
        // win11: the two buttons are separate rounded boxes
        RECT rs = _rcShut;
        if (_w11) rs.right -= DPI_SX(2);
        button(rs, _hot == H_SHUT);
        SetTextColor(hdc, _clrRightText);
        DrawTextW(hdc, L"Shut down", -1, &rs, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
    button(_rcArrow, _hot == H_ARROW);
    SetTextColor(hdc, _clrRightText);
    DrawTextW(hdc, L"\x25B8", -1, &_rcArrow, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

void StartMenu7::Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(_hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, _font);
    if (_w11)
        FillColor(mem, rc, _clrLeft);   // the band above the right column (search row)
    DrawLeft(mem);
    DrawRight(mem);
    if (_w11) {
        DrawSearch(mem);
        // with the rounded region the 1 px rectangle frame would be cut at the corners
        W11_FrameRound(mem, rc, DPI_SX(8), _clrBorder);
    } else
        FrameColor(mem, rc, _clrBorder);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------------------------------------------------------- actions

void StartMenu7::RunCommand(const std::wstring &cmd, const std::wstring &params)
{
    Hide();
    if (cmd == L"::explorer")
        explorer_show_frame(SW_SHOWNORMAL);
    else if (cmd == L"::run")
        ShowLaunchDialog(g_Globals._hwndDesktopBar);
    else if (cmd == L"::restart")
        ShowRestartWindowsDialog(g_Globals._hwndDesktop);
    else if (cmd == L"::shutdown")
        ShowExitWindowsDialog(g_Globals._hwndDesktop);
    else if (!cmd.empty())
        launch_file(g_Globals._hwndDesktop, cmd.c_str(), SW_SHOWNORMAL, params.empty() ? NULL : params.c_str());
}

void StartMenu7::Activate(int idx)
{
    if (idx == (int)_rows.size()) {                     // footer: All Programs / Back
        if (_mode == M_SEARCH)
            SetWindowText(_edit, TEXT(""));             // EN_CHANGE -> home
        else
            SetMode(_mode == M_HOME ? M_ALL : M_HOME);
        return;
    }
    if (idx < 0 || idx >= (int)_rows.size()) {
        if (_mode == M_SEARCH) {                        // no match: run the typed text (like Run...)
            wchar_t text[1024];
            GetWindowTextW(_edit, text, 1024);
            std::wstring t = text, file, params;
            t.erase(0, t.find_first_not_of(L' '));
            if (t.empty())
                return;
            // like Run...: an existing path as a whole, else "<program> <parameters>" ("quoted program" allowed)
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES || t.find(L' ') == std::wstring::npos) {
                file = t;
            } else if (t[0] == L'"') {
                size_t q = t.find(L'"', 1);
                file = t.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
                if (q != std::wstring::npos && q + 1 < t.size())
                    params = t.substr(t.find_first_not_of(L' ', q + 1) == std::wstring::npos ? t.size() : t.find_first_not_of(L' ', q + 1));
            } else {
                size_t sp = t.find(L' ');
                file = t.substr(0, sp);
                size_t p = t.find_first_not_of(L' ', sp);
                if (p != std::wstring::npos)
                    params = t.substr(p);
            }
            Hide();
            launch_file(g_Globals._hwndDesktop, file.c_str(), SW_SHOWNORMAL, params.empty() ? NULL : params.c_str());
        }
        return;
    }
    Row &r = _rows[idx];
    if (r.kind != R_ITEM) return;
    if (r.node->folder) {
        r.node->expanded = !r.node->expanded;
        BuildRows();
        EnsureVisible(_sel);
        InvalidateRect(_hwnd, NULL, FALSE);
        return;
    }
    std::wstring path = r.node->paths[0];
    Hide();
    launch_file(g_Globals._hwndDesktop, path.c_str(), SW_SHOWNORMAL);
}

void StartMenu7::ActivateRight(int idx)
{
    if (idx >= 0 && idx < (int)_right.size() && !_right[idx].sep)
        RunCommand(_right[idx].command, _right[idx].parameters);
}

void StartMenu7::ShowShutdownMenu()
{
    HMENU m = CreatePopupMenu();
    if (!JCFG2_DEF("JS_STARTMENU", "norestart", false).ToBool())
        AppendMenu(m, MF_STRING, ID_SM7_RESTART, TEXT("Restart..."));
    AppendMenu(m, MF_STRING, ID_SM7_RESTARTSHELL, TEXT("Restart shell"));
    if (JCFG2_DEF("JS_STARTMENU", "noshutdown", false).ToBool() == false) {
        AppendMenu(m, MF_SEPARATOR, 0, NULL);
        AppendMenu(m, MF_STRING, ID_SM7_SHUTDOWN, TEXT("Shut down..."));
    }
    POINT pt = {_rcArrow.right, _rcArrow.top};
    ClientToScreen(_hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, _hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case ID_SM7_RESTART:      RunCommand(L"::restart", L""); break;
    case ID_SM7_SHUTDOWN:     RunCommand(L"::shutdown", L""); break;
    case ID_SM7_RESTARTSHELL: Hide(); PostMessage(g_Globals._hwndDesktop, PM_MYPE_RESTART_SHELL, 0, 0); break;
    }
}

void StartMenu7::SetMode(Mode m)
{
    _mode = m;
    _scroll = 0;
    _sel = -1;
    if (m != M_SEARCH)
        _matches.clear();
    BuildRows();
    if (m == M_SEARCH && !_matches.empty())
        _sel = 0;
    InvalidateRect(_hwnd, NULL, FALSE);
}

void StartMenu7::OnSearchChanged()
{
    wchar_t text[256];
    GetWindowTextW(_edit, text, 256);
    std::wstring q = text;
    q.erase(0, q.find_first_not_of(L' '));
    if (q.empty()) {
        SetMode(M_HOME);
        return;
    }
    _matches.clear();
    CollectMatches(_root, q, _matches);
    SetMode(M_SEARCH);
}

bool StartMenu7::OnKey(UINT vk)
{
    int last = (int)_rows.size();                       // index of the footer (not in search mode)
    switch (vk) {
    case VK_ESCAPE:
        if (GetWindowTextLength(_edit) > 0)
            SetWindowText(_edit, TEXT(""));             // EN_CHANGE -> home
        else if (_mode == M_ALL)
            SetMode(M_HOME);
        else
            Hide();
        return true;
    case VK_RETURN:
        Activate(_mode == M_SEARCH && _sel < 0 ? -1 : _sel);
        return true;
    case VK_DOWN:
    case VK_UP: {
        int maxIdx = _mode == M_SEARCH ? last - 1 : last;
        int i = _sel;
        do {
            i += vk == VK_DOWN ? 1 : -1;
            if (i < 0) i = maxIdx;
            if (i > maxIdx) i = 0;
        } while (i < last && _rows[i].kind != R_ITEM && i != _sel);
        _sel = i;
        EnsureVisible(_sel);
        InvalidateRect(_hwnd, NULL, FALSE);
        return true;
    }
    case VK_RIGHT:
    case VK_LEFT:
        if (_mode == M_ALL && _sel >= 0 && _sel < last && _rows[_sel].node->folder) {
            bool want = vk == VK_RIGHT;
            if (_rows[_sel].node->expanded != want) Activate(_sel);
            return true;
        }
        return false;                                   // caret movement in the search box
    }
    return false;
}

void StartMenu7::SetHot(HotArea a, int idx)
{
    if (a == _hot && idx == _hotIdx) return;
    _hot = a;
    _hotIdx = idx;
    InvalidateRect(_hwnd, NULL, FALSE);
}

void StartMenu7::Toggle(HWND hwndStartButton)
{
    if (!_hwnd)
        Create();
    if (Visible()) {
        Hide();
        return;
    }
    if (GetTickCount() - _hiddenAt < 250)               // this click on Start is what closed us (deactivation)
        return;
    _startBtn = hwndStartButton;
    s_extraNotify = _hwnd;
    StartExtraScan();                   // result arrives as PM_SM7_EXTRA_DONE
    LoadConfig();
    Rescan();
    Layout();
    SetWindowText(_edit, TEXT(""));
    _hot = H_NONE;
    SetMode(M_HOME);
    ShowWindow(_hwnd, SW_SHOW);
    SetForegroundWindow(_hwnd);
    SetFocus(_edit);
}

void StartMenu7::Hide()
{
    if (Visible()) {
        ShowWindow(_hwnd, SW_HIDE);
        _hiddenAt = GetTickCount();
    }
}

// ---------------------------------------------------------------- window procedures

LRESULT CALLBACK StartMenu7::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    StartMenu7 *self = (StartMenu7 *)ref;
    if (msg == WM_KEYDOWN && self->OnKey((UINT)wp))
        return 0;
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
        return 0;                                       // no beep
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK StartMenu7::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    StartMenu7 &self = Get();
    if (self._hwnd == hwnd)
        return self.Proc(msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT StartMenu7::Proc(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(_hwnd, &ps);
        Paint(hdc);
        EndPaint(_hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case PM_SM7_EXTRA_DONE:             // background scan of extra_folders finished
        if (Visible() && _mode != M_ALL) {   // All Programs: keep the expanded folders, applied on next open
            Rescan();
            if (_mode == M_SEARCH)
                OnSearchChanged();
            else
                SetMode(M_HOME);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            Hide();
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SM7_SEARCH && HIWORD(wp) == EN_CHANGE)
            OnSearchChanged();
        return 0;

    case WM_CTLCOLOREDIT:               // win11: the search box in the theme colors (dark: light text on dark)
        if (_w11 && _brSearch) {
            SetTextColor((HDC)wp, _clrLeftText);
            SetBkColor((HDC)wp, _clrSearch);
            return (LRESULT)_brSearch;
        }
        break;

    case WM_MOUSEMOVE: {
        if (!_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, _hwnd, 0 };
            _tracking = TrackMouseEvent(&tme) != FALSE;
        }
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int row = RowAt(pt);
        if (row >= 0 && row != _sel) {
            _sel = row;
            InvalidateRect(_hwnd, NULL, FALSE);
        }
        if (PtInRect(&_rcFooter, pt)) SetHot(H_FOOTER, 0);
        else if (PtInRect(&_rcShut, pt) && !JCFG2_DEF("JS_STARTMENU", "noshutdown", false).ToBool()) SetHot(H_SHUT, 0);
        else if (PtInRect(&_rcArrow, pt)) SetHot(H_ARROW, 0);
        else {
            int ri = RightAt(pt);
            SetHot(ri >= 0 ? H_RIGHT : H_NONE, ri);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        _tracking = false;
        SetHot(H_NONE, -1);
        return 0;

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int row = RowAt(pt);
        if (row >= 0) { _sel = row; Activate(row); }
        else if (PtInRect(&_rcFooter, pt)) Activate((int)_rows.size());
        else if (PtInRect(&_rcShut, pt)) { if (!JCFG2_DEF("JS_STARTMENU", "noshutdown", false).ToBool()) RunCommand(L"::shutdown", L""); }
        else if (PtInRect(&_rcArrow, pt)) ShowShutdownMenu();
        else ActivateRight(RightAt(pt));
        if (Visible()) SetFocus(_edit);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        _scroll -= delta * (_mode == M_HOME ? _rowLarge : _rowSmall) * 3 / WHEEL_DELTA;
        ClampScroll();
        InvalidateRect(_hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProc(_hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------- API

bool StartMenu7_Enabled()
{
    std::wstring style = JCFG2_DEF("JS_STARTMENU", "style", TEXT("classic")).ToString(std::wstring(L"classic"));
    return !_wcsicmp(style.c_str(), L"win7") || !_wcsicmp(style.c_str(), L"win11");
}

void StartMenu7_Toggle(HWND hwndStartButton) { StartMenu7::Get().Toggle(hwndStartButton); }
bool StartMenu7_IsVisible() { return StartMenu7::Get().Visible(); }
void StartMenu7_Hide() { StartMenu7::Get().Hide(); }
void StartMenu7_Prefetch() { if (StartMenu7_Enabled()) StartExtraScan(); }
