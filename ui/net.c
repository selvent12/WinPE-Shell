/*
 * net.c - MyPE network control (docs/FEATURES.md: network OFF at boot, started manually). Own code, Win32 only.
 *
 *   MyPEUI.exe net on|off|toggle|status [/quiet] [/boot]
 *   MyPEUI.exe tray-net                  tray icon (single instance): OFF / ON / connected
 *   MyPEUI.exe open network              "Network" window: ON/OFF, adapters, Wi-Fi list + connect (also ms-availablenetworks:,
 *                                        ms-settings:network*)
 *
 * ON  = enable physical NICs (if "net off" disabled them), "wpeutil EnableFirewall", start [network] onServices in order
 *       (the SCM starts their DependOnService chain), "wpeutil InitializeNetwork" (installs the NIC devices), then
 *       "wpeutil WaitForNetwork" and a wait for a DHCP lease, both limited to waitSeconds.
 * OFF = disconnect Wi-Fi, "ipconfig /release" (if a lease exists), disable physical NICs (until reboot), stop
 *       offServices in order (active dependents first), verify: no IPv4 address left, services stopped.
 * /boot (startup step after drvload): the same checks; makes sure the image starts in the OFF state and logs it.
 * Settings: X:\MyPE\config\mypeui.ini [network] mode=manual|full-off|noNicKeepTcpip, onServices=, offServices=,
 *           waitSeconds= (written by the builder from build.json). Intent: HKLM\SOFTWARE\MyPE\Network State=on|off|
 *           turning-on|turning-off (RAM registry). Every step goes to X:\MyPE\logs\mypeui.log ("net: ..."); console
 *           output of wpeutil/ipconfig to X:\MyPE\logs\net-commands.log.
 *
 * Wi-Fi uses the Native Wifi API (wlanapi.dll loaded at run time, so MyPEUI.exe also starts in images without WLAN):
 * WlanOpenHandle / WlanEnumInterfaces / WlanScan / WlanGetAvailableNetworkList / WlanConnect with a TEMPORARY profile
 * (wlan_connection_mode_temporary_profile: built in memory from SSID + password, never stored by WlanSvc, buffers wiped)
 * / WlanDisconnect. WPA2-Personal (AES/TKIP), WPA3-Personal (SAE) and open networks; 802.1X / WEP are refused.
 *
 * Control IDs (stable, used by tests\vm\actions\network.ps1):
 *   401 status  402 ON/OFF button  403 adapters text  404 "Wi-Fi" header  405 network list (ListView)  406 password label
 *   407 password  408 Connect  409 Disconnect  410 Refresh  411 Close  412 Wi-Fi message  413 Wi-Fi connection status
 * Tray window: class MyPE.TrayNet, title "MyPE network: <off|on|connected|turning-on|turning-off> <IPv4 list>".
 */
#include "mypeui.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <windns.h>
#include <wlanapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

#define NET_REGKEY     L"SOFTWARE\\MyPE\\Network"
#define NET_OPMUTEX    L"Global\\MyPE.NetOperation"
#define TRAY_MUTEX     L"Local\\MyPE.TrayNet"
#define TRAY_CLASS     L"MyPE.TrayNet"
#define NETW_CLASS     L"MyPE.Network"
#define NETW_MUTEX     L"Local\\MyPE.Network"
#define CMDLOG         L"X:\\MyPE\\logs\\net-commands.log"
#define WM_TRAYCB      (WM_APP + 1)
#define WM_APP_WLAN    (WM_APP + 2)
#define MAXAD          16
#define MAXNETS        64

#ifndef DOT11_AUTH_ALGO_WPA3_SAE
#define DOT11_AUTH_ALGO_WPA3_SAE ((DOT11_AUTH_ALGORITHM)9)
#endif

enum { NS_OFF = 0, NS_ON, NS_CONNECTED, NS_TURNING_ON, NS_TURNING_OFF };
static const wchar_t *s_stateName[] = { L"off", L"on", L"connected", L"turning-on", L"turning-off" };

/* exit codes of "net on|off" (also shown by the tray) */
enum { NRC_OK = 0, NRC_NOTMANUAL = 3, NRC_BUSY = 4, NRC_NOADAPTER = 5, NRC_NOLEASE = 6, NRC_NOTVERIFIED = 7 };

static const GUID kNetClass = { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

typedef struct { wchar_t mode[32]; wchar_t on[1024]; wchar_t off[1024]; wchar_t bootDis[256]; wchar_t dnsName[256]; DWORD wait; } NetCfg;
typedef struct { wchar_t name[96]; wchar_t desc[160]; wchar_t ipv4[160]; wchar_t gw[48]; wchar_t dhcp[48]; wchar_t dns[100]; DWORD type; BOOL up; int nIp, nLease; } NetAdapter;

/* ======================================================================================================== */
/* helpers                                                                                                   */
/* ======================================================================================================== */

static void NetLoadCfg(NetCfg *c)
{
    ZeroMemory(c, sizeof(*c));
    GetPrivateProfileStringW(L"network", L"mode", L"", c->mode, _countof(c->mode), MYPE_INI);
    GetPrivateProfileStringW(L"network", L"onServices", L"BFE,MpsSvc,nsi,Dhcp,Dnscache,NlaSvc,lmhosts,LanmanWorkstation,WlanSvc", c->on, _countof(c->on), MYPE_INI);
    GetPrivateProfileStringW(L"network", L"offServices", L"WlanSvc,LanmanWorkstation,lmhosts,NlaSvc,Dhcp", c->off, _countof(c->off), MYPE_INI);
    /* bootDisabled: Start=4 in the image (trigger-started otherwise, e.g. Dnscache), switched to demand-start by "net on"
       and back to disabled by "net off" (Dnscache refuses SERVICE_STOP by design: it then stays running, without IP) */
    GetPrivateProfileStringW(L"network", L"bootDisabled", L"", c->bootDis, _countof(c->bootDis), MYPE_INI);
    /* dnsTestName: optional public name resolved after "net on" as a DNS check (empty = only the local computer name) */
    GetPrivateProfileStringW(L"network", L"dnsTestName", L"", c->dnsName, _countof(c->dnsName), MYPE_INI);
    c->wait = GetPrivateProfileIntW(L"network", L"waitSeconds", 60, MYPE_INI);
    if (c->wait < 5) c->wait = 5;
    if (c->wait > 600) c->wait = 600;
}

static BOOL NetManual(const NetCfg *c) { return !_wcsicmp(c->mode, L"manual"); }

static void NetSetState(const wchar_t *s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, NET_REGKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"State", 0, REG_SZ, (const BYTE *)s, (DWORD)((wcslen(s) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static void NetGetState(wchar_t *s, DWORD cch)
{
    DWORD cb = cch * sizeof(wchar_t), type = 0;
    s[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, NET_REGKEY, L"State", RRF_RT_REG_SZ, &type, s, &cb) != ERROR_SUCCESS) s[0] = 0;
}

/* runs a console program hidden, stdout/stderr appended to CMDLOG; returns the exit code, or (DWORD)-1 */
static DWORD RunWait(const wchar_t *cmdline, DWORD timeoutMs, BOOL *timedOut)
{
    wchar_t line[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE out;
    DWORD rc = (DWORD)-1, n;
    char hdr[1200];
    int len;

    if (timedOut) *timedOut = FALSE;
    wcsncpy_s(line, _countof(line), cmdline, _TRUNCATE);
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    CreateDirectoryW(MYPE_LOGDIR, NULL);
    out = CreateFileW(CMDLOG, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        len = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE, "\r\n==== %02u:%02u:%02u %ls\r\n", t.wHour, t.wMinute, t.wSecond, cmdline);
        if (len > 0) WriteFile(out, hdr, (DWORD)len, &n, NULL);
    }
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (out != INVALID_HANDLE_VALUE) { si.dwFlags |= STARTF_USESTDHANDLES; si.hStdOutput = out; si.hStdError = out; si.hStdInput = NULL; }
    if (!CreateProcessW(NULL, line, NULL, NULL, out != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log(L"net: start FAILED err=%lu: %s", GetLastError(), cmdline);
    } else {
        CloseHandle(pi.hThread);
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &rc);
        else { if (timedOut) *timedOut = TRUE; TerminateProcess(pi.hProcess, 1460); WaitForSingleObject(pi.hProcess, 5000); }
        CloseHandle(pi.hProcess);
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return rc;
}

static DWORD Wpeutil(const wchar_t *verb, DWORD timeoutMs)
{
    wchar_t cmd[MAX_PATH + 64], sys[MAX_PATH];
    BOOL to = FALSE;
    DWORD rc, t0 = GetTickCount();
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\\wpeutil.exe\" %s", sys, verb);
    rc = RunWait(cmd, timeoutMs, &to);
    Log(L"net: wpeutil %s -> rc=%ld%s (%lu ms)", verb, (long)rc, to ? L" TIMEOUT (terminated)" : L"", GetTickCount() - t0);
    return to ? WAIT_TIMEOUT : rc;
}

/* ---- services ---- */

static DWORD SvcCurrent(SC_HANDLE scm, const wchar_t *name)
{
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    SERVICE_STATUS st;
    DWORD r = 0;
    if (!s) return 0;
    if (QueryServiceStatus(s, &st)) r = st.dwCurrentState;
    CloseServiceHandle(s);
    return r;
}

static BOOL SvcWait(SC_HANDLE s, DWORD want, DWORD timeoutMs, DWORD *last)
{
    SERVICE_STATUS st;
    DWORD t0 = GetTickCount();
    for (;;) {
        if (!QueryServiceStatus(s, &st)) { *last = 0; return FALSE; }
        *last = st.dwCurrentState;
        if (st.dwCurrentState == want) return TRUE;
        if (want == SERVICE_RUNNING && st.dwCurrentState == SERVICE_STOPPED) return FALSE;
        if (GetTickCount() - t0 > timeoutMs) return FALSE;
        Sleep(200);
    }
}

/* returns TRUE if running afterwards; *missing = service not in the image */
static BOOL SvcStart(SC_HANDLE scm, const wchar_t *name, BOOL *missing)
{
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    DWORD last = 0, t0 = GetTickCount(), err;
    BOOL ok;
    *missing = FALSE;
    if (!s) {
        err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) { *missing = TRUE; Log(L"net: service %s: not in this image (skipped)", name); }
        else Log(L"net: service %s: OpenService FAILED err=%lu", name, err);
        return FALSE;
    }
    if (SvcCurrent(scm, name) == SERVICE_RUNNING) { Log(L"net: service %s: already running", name); CloseServiceHandle(s); return TRUE; }
    if (!StartServiceW(s, 0, NULL) && (err = GetLastError()) != ERROR_SERVICE_ALREADY_RUNNING) {
        Log(L"net: service %s: StartService FAILED err=%lu", name, err);
        CloseServiceHandle(s);
        return FALSE;
    }
    ok = SvcWait(s, SERVICE_RUNNING, 30000, &last);
    {
        SERVICE_STATUS_PROCESS sp;
        DWORD need = 0, ex = 0, sx = 0;
        if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)&sp, sizeof(sp), &need)) { ex = sp.dwWin32ExitCode; sx = sp.dwServiceSpecificExitCode; }
        Log(L"net: service %s: %s (state %lu, exit %lu/%lu, %lu ms)", name, ok ? L"started" : L"NOT running", last, ex, sx, GetTickCount() - t0);
    }
    CloseServiceHandle(s);
    return ok;
}

static void SvcStopTree(SC_HANDLE scm, const wchar_t *name, int depth)
{
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    SERVICE_STATUS st;
    DWORD last = 0, need = 0, cnt = 0, i, t0 = GetTickCount();
    BYTE buf[8192];
    if (!s) { if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) Log(L"net: service %s: OpenService (stop) FAILED err=%lu", name, GetLastError()); return; }
    if (!QueryServiceStatus(s, &st) || st.dwCurrentState == SERVICE_STOPPED) { CloseServiceHandle(s); return; }
    if (depth < 4 && EnumDependentServicesW(s, SERVICE_ACTIVE, (LPENUM_SERVICE_STATUSW)buf, sizeof(buf), &need, &cnt)) {
        LPENUM_SERVICE_STATUSW d = (LPENUM_SERVICE_STATUSW)buf;
        for (i = 0; i < cnt; i++) { Log(L"net: service %s: stopping dependent %s first", name, d[i].lpServiceName); SvcStopTree(scm, d[i].lpServiceName, depth + 1); }
    }
    if (!ControlService(s, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        DWORD e = GetLastError();
        SERVICE_STATUS q;   /* st is undefined after a failed ControlService: read the state again */
        Log(L"net: service %s: stop FAILED err=%lu (state %lu)", name, e, QueryServiceStatus(s, &q) ? q.dwCurrentState : 0);
        CloseServiceHandle(s);
        return;
    }
    Log(L"net: service %s: %s (%lu ms)", name, SvcWait(s, SERVICE_STOPPED, 20000, &last) ? L"stopped" : L"still not stopped", GetTickCount() - t0);
    CloseServiceHandle(s);
}

static void SvcSetStart(SC_HANDLE scm, const wchar_t *name, DWORD start)
{
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_CHANGE_CONFIG);
    if (!s) { if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) Log(L"net: service %s: OpenService (config) FAILED err=%lu", name, GetLastError()); return; }
    if (ChangeServiceConfigW(s, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
        Log(L"net: service %s: start type -> %s", name, start == SERVICE_DISABLED ? L"disabled" : L"demand");
    else Log(L"net: service %s: ChangeServiceConfig FAILED err=%lu", name, GetLastError());
    CloseServiceHandle(s);
}

/* TRUE if name is in the comma/space separated list */
static BOOL InList(const wchar_t *list, const wchar_t *name)
{
    wchar_t buf[1024], *tok, *ctx = NULL;
    wcsncpy_s(buf, _countof(buf), list, _TRUNCATE);
    for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) if (!_wcsicmp(tok, name)) return TRUE;
    return FALSE;
}

static void SvcSetStartList(SC_HANDLE scm, const wchar_t *list, DWORD start)
{
    wchar_t buf[256], *tok, *ctx = NULL;
    wcsncpy_s(buf, _countof(buf), list, _TRUNCATE);
    for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) SvcSetStart(scm, tok, start);
}

/* ---- adapters (GetAdaptersAddresses) ---- */

static void Ip4(const SOCKADDR *sa, wchar_t *s, size_t cch)
{
    const BYTE *b;
    s[0] = 0;
    if (!sa || sa->sa_family != AF_INET) return;
    b = (const BYTE *)&((const SOCKADDR_IN *)sa)->sin_addr;
    _snwprintf_s(s, cch, _TRUNCATE, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* physical-ish adapters (no loopback / tunnel); returns count */
static int NetAdapters(NetAdapter *a, int max)
{
    ULONG sz = 32768, rc;
    IP_ADAPTER_ADDRESSES *buf = NULL, *p;
    int n = 0, k;
    for (k = 0; k < 3; k++) {
        buf = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!buf) return 0;
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST, NULL, buf, &sz);
        if (rc == ERROR_SUCCESS) break;
        free(buf); buf = NULL;
        if (rc != ERROR_BUFFER_OVERFLOW) return 0;
    }
    if (!buf) return 0;
    for (p = buf; p && n < max; p = p->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        NetAdapter *x;
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK || p->IfType == IF_TYPE_TUNNEL) continue;
        x = &a[n++];
        ZeroMemory(x, sizeof(*x));
        wcsncpy_s(x->name, _countof(x->name), p->FriendlyName ? p->FriendlyName : L"", _TRUNCATE);
        wcsncpy_s(x->desc, _countof(x->desc), p->Description ? p->Description : L"", _TRUNCATE);
        x->type = p->IfType; x->up = p->OperStatus == IfOperStatusUp;
        for (u = p->FirstUnicastAddress; u; u = u->Next) {
            wchar_t ip[24];
            Ip4(u->Address.lpSockaddr, ip, _countof(ip));
            if (!ip[0]) continue;
            x->nIp++;
            if (wcsncmp(ip, L"169.254.", 8)) x->nLease++;
            if (x->ipv4[0]) wcscat_s(x->ipv4, _countof(x->ipv4), L", ");
            if (wcslen(x->ipv4) + wcslen(ip) + 2 < _countof(x->ipv4)) wcscat_s(x->ipv4, _countof(x->ipv4), ip);
        }
        if (p->FirstGatewayAddress) {
            IP_ADAPTER_GATEWAY_ADDRESS_LH *g;
            for (g = p->FirstGatewayAddress; g && !x->gw[0]; g = g->Next) Ip4(g->Address.lpSockaddr, x->gw, _countof(x->gw));
        }
        Ip4(p->Dhcpv4Server.lpSockaddr, x->dhcp, _countof(x->dhcp));
        {
            IP_ADAPTER_DNS_SERVER_ADDRESS *d;
            for (d = p->FirstDnsServerAddress; d; d = d->Next) {
                wchar_t ip[24];
                Ip4(d->Address.lpSockaddr, ip, _countof(ip));
                if (!ip[0] || wcslen(x->dns) + wcslen(ip) + 2 >= _countof(x->dns)) continue;
                if (x->dns[0]) wcscat_s(x->dns, _countof(x->dns), L" ");
                wcscat_s(x->dns, _countof(x->dns), ip);
            }
        }
    }
    free(buf);
    return n;
}

/* IPv4 addresses on non-loopback adapters: *leases = without 169.254.x.x; list = "a.b.c.d, ..." */
static int NetIPv4(int *leases, wchar_t *list, size_t cch)
{
    NetAdapter a[MAXAD];
    int n = NetAdapters(a, MAXAD), i, ips = 0;
    if (leases) *leases = 0;
    if (list) list[0] = 0;
    for (i = 0; i < n; i++) {
        ips += a[i].nIp;
        if (leases) *leases += a[i].nLease;
        if (list && a[i].ipv4[0]) { if (list[0]) wcscat_s(list, cch, L" "); if (wcslen(list) + wcslen(a[i].ipv4) + 1 < cch) wcscat_s(list, cch, a[i].ipv4); }
    }
    return ips;
}

/* ---- NIC devices (SetupAPI + CfgMgr32): enable/disable the physical ones ---- */

static int NetDevices(BOOL enable, BOOL dryRun)
{
    HDEVINFO di = SetupDiGetClassDevsW(&kNetClass, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA dd;
    DWORD i;
    int phys = 0;
    if (di == INVALID_HANDLE_VALUE) { Log(L"net: SetupDiGetClassDevs FAILED err=%lu", GetLastError()); return 0; }
    for (i = 0; ; i++) {
        wchar_t id[400], fn[256];
        ULONG st = 0, prob = 0;
        CONFIGRET cr;
        dd.cbSize = sizeof(dd);
        if (!SetupDiEnumDeviceInfo(di, i, &dd)) break;
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, id, _countof(id), NULL)) continue;
        /* software adapters: ROOT\ (WAN miniports, KDNIC), SWD\, {...} enumerators */
        if (!_wcsnicmp(id, L"ROOT\\", 5) || !_wcsnicmp(id, L"SWD\\", 4) || id[0] == L'{') continue;
        phys++;
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_FRIENDLYNAME, NULL, (PBYTE)fn, sizeof(fn), NULL) &&
            !SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_DEVICEDESC, NULL, (PBYTE)fn, sizeof(fn), NULL)) fn[0] = 0;
        cr = CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0);
        if (dryRun) { Log(L"net: NIC %s [%s] status=0x%lx problem=%lu", fn, id, cr == CR_SUCCESS ? st : 0, cr == CR_SUCCESS ? prob : 0); continue; }
        if (enable) {
            if (cr == CR_SUCCESS && (st & DN_HAS_PROBLEM) && prob == CM_PROB_DISABLED) {
                cr = CM_Enable_DevNode(dd.DevInst, 0);
                Log(L"net: NIC %s [%s] enable -> cr=%lu", fn, id, cr);
            } else Log(L"net: NIC %s [%s] enabled already (status=0x%lx problem=%lu)", fn, id, st, prob);
        } else {
            if (cr == CR_SUCCESS && (st & DN_HAS_PROBLEM) && prob == CM_PROB_DISABLED) Log(L"net: NIC %s [%s] disabled already", fn, id);
            else { cr = CM_Disable_DevNode(dd.DevInst, CM_DISABLE_UI_NOT_OK); Log(L"net: NIC %s [%s] disable (until reboot) -> cr=%lu", fn, id, cr); }
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return phys;
}

/* Network controllers without a driver: wpeinit with EnableNetwork=false does not install devices and "wpeutil
   InitializeNetwork" then returns at once (VM run B 2026-11-24: e1000e stayed "problem 18", no adapter). Installs the
   best driver from the image's driver store (DiInstallDevice, no INF given) on every present PCI/USB device without a
   driver whose IDs say "network controller" (PCI class 02: Ethernet 0200, Wi-Fi 0280; USB class E0 = RNDIS/wireless).
   Returns the number of devices installed. */
static int NetInstallNics(void)
{
    HDEVINFO di = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dd;
    DWORD i;
    int done = 0;
    if (di == INVALID_HANDLE_VALUE) { Log(L"net: SetupDiGetClassDevs (all) FAILED err=%lu", GetLastError()); return 0; }
    for (i = 0; ; i++) {
        wchar_t id[400], ids[2048], fn[256], *p;
        ULONG st = 0, prob = 0;
        BOOL net = FALSE, reboot = FALSE;
        DWORD k;
        dd.cbSize = sizeof(dd);
        if (!SetupDiEnumDeviceInfo(di, i, &dd)) break;
        if (CM_Get_DevNode_Status(&st, &prob, dd.DevInst, 0) != CR_SUCCESS) continue;
        if (!(st & DN_HAS_PROBLEM) || (prob != CM_PROB_NOT_CONFIGURED && prob != CM_PROB_REINSTALL && prob != CM_PROB_FAILED_INSTALL)) continue;
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, id, _countof(id), NULL)) continue;
        for (k = 0; k < 2 && !net; k++) {
            ZeroMemory(ids, sizeof(ids));
            if (!SetupDiGetDeviceRegistryPropertyW(di, &dd, k ? SPDRP_COMPATIBLEIDS : SPDRP_HARDWAREID, NULL, (PBYTE)ids, sizeof(ids) - 4, NULL)) continue;
            for (p = ids; *p; p += wcslen(p) + 1) {
                wchar_t up[260];
                wcsncpy_s(up, _countof(up), p, _TRUNCATE); _wcsupr_s(up, _countof(up));
                if ((!wcsncmp(up, L"PCI\\", 4) && wcsstr(up, L"CC_02")) || (!wcsncmp(up, L"USB\\", 4) && wcsstr(up, L"CLASS_E0"))) { net = TRUE; break; }
            }
        }
        if (!net) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_DEVICEDESC, NULL, (PBYTE)fn, sizeof(fn), NULL)) fn[0] = 0;
        if (DiInstallDevice(NULL, di, &dd, NULL, 0, &reboot)) { done++; Log(L"net: network controller %s [%s] (problem %lu): driver installed from the driver store%s", fn, id, prob, reboot ? L" (reboot requested)" : L""); }
        else Log(L"net: network controller %s [%s] (problem %lu): no driver installed, err=0x%08lx (no matching driver in MyPE?)", fn, id, prob, GetLastError());
    }
    SetupDiDestroyDeviceInfoList(di);
    return done;
}

/* ---- Wi-Fi API (dynamic) ---- */
typedef DWORD (WINAPI *PFN_WlanOpenHandle)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD (WINAPI *PFN_WlanCloseHandle)(HANDLE, PVOID);
typedef DWORD (WINAPI *PFN_WlanEnumInterfaces)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
typedef DWORD (WINAPI *PFN_WlanScan)(HANDLE, const GUID *, const PDOT11_SSID, const PWLAN_RAW_DATA, PVOID);
typedef DWORD (WINAPI *PFN_WlanGetAvailableNetworkList)(HANDLE, const GUID *, DWORD, PVOID, PWLAN_AVAILABLE_NETWORK_LIST *);
typedef DWORD (WINAPI *PFN_WlanConnect)(HANDLE, const GUID *, const PWLAN_CONNECTION_PARAMETERS, PVOID);
typedef DWORD (WINAPI *PFN_WlanDisconnect)(HANDLE, const GUID *, PVOID);
typedef VOID  (WINAPI *PFN_WlanFreeMemory)(PVOID);
typedef DWORD (WINAPI *PFN_WlanRegisterNotification)(HANDLE, DWORD, BOOL, WLAN_NOTIFICATION_CALLBACK, PVOID, PVOID, PDWORD);
typedef DWORD (WINAPI *PFN_WlanReasonCodeToString)(DWORD, DWORD, PWCHAR, PVOID);

static struct {
    HMODULE dll;
    PFN_WlanOpenHandle Open; PFN_WlanCloseHandle Close; PFN_WlanEnumInterfaces Enum; PFN_WlanScan Scan;
    PFN_WlanGetAvailableNetworkList List; PFN_WlanConnect Connect; PFN_WlanDisconnect Disconnect; PFN_WlanFreeMemory Free;
    PFN_WlanRegisterNotification Notify; PFN_WlanReasonCodeToString Reason;
} W;

static BOOL WlanLoad(void)
{
    if (W.dll) return W.Open != NULL;
    W.dll = LoadLibraryExW(L"wlanapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!W.dll) { Log(L"net: wlanapi.dll not loadable err=%lu (no WLAN support in this image)", GetLastError()); return FALSE; }
    W.Open = (PFN_WlanOpenHandle)(void *)GetProcAddress(W.dll, "WlanOpenHandle");
    W.Close = (PFN_WlanCloseHandle)(void *)GetProcAddress(W.dll, "WlanCloseHandle");
    W.Enum = (PFN_WlanEnumInterfaces)(void *)GetProcAddress(W.dll, "WlanEnumInterfaces");
    W.Scan = (PFN_WlanScan)(void *)GetProcAddress(W.dll, "WlanScan");
    W.List = (PFN_WlanGetAvailableNetworkList)(void *)GetProcAddress(W.dll, "WlanGetAvailableNetworkList");
    W.Connect = (PFN_WlanConnect)(void *)GetProcAddress(W.dll, "WlanConnect");
    W.Disconnect = (PFN_WlanDisconnect)(void *)GetProcAddress(W.dll, "WlanDisconnect");
    W.Free = (PFN_WlanFreeMemory)(void *)GetProcAddress(W.dll, "WlanFreeMemory");
    W.Notify = (PFN_WlanRegisterNotification)(void *)GetProcAddress(W.dll, "WlanRegisterNotification");
    W.Reason = (PFN_WlanReasonCodeToString)(void *)GetProcAddress(W.dll, "WlanReasonCodeToString");
    if (!W.Open || !W.Close || !W.Enum || !W.Scan || !W.List || !W.Connect || !W.Disconnect || !W.Free) {
        Log(L"net: wlanapi.dll lacks expected exports");
        W.Open = NULL;
        return FALSE;
    }
    return TRUE;
}

static void WlanDisconnectAll(void)
{
    HANDLE h = NULL;
    DWORD ver = 0, rc, i;
    PWLAN_INTERFACE_INFO_LIST il = NULL;
    if (!WlanLoad()) return;
    if ((rc = W.Open(2, NULL, &ver, &h)) != ERROR_SUCCESS) { Log(L"net: Wi-Fi: WlanOpenHandle rc=%lu (service not running?)", rc); return; }
    if (W.Enum(h, NULL, &il) == ERROR_SUCCESS && il) {
        for (i = 0; i < il->dwNumberOfItems; i++) {
            if (il->InterfaceInfo[i].isState == wlan_interface_state_connected || il->InterfaceInfo[i].isState == wlan_interface_state_associating ||
                il->InterfaceInfo[i].isState == wlan_interface_state_authenticating) {
                rc = W.Disconnect(h, &il->InterfaceInfo[i].InterfaceGuid, NULL);
                Log(L"net: Wi-Fi: disconnect '%s' rc=%lu", il->InterfaceInfo[i].strInterfaceDescription, rc);
            }
        }
        Log(L"net: Wi-Fi: %lu WLAN interface(s)", il->dwNumberOfItems);
        W.Free(il);
    }
    W.Close(h, NULL);
}

/* DNS check after "net on" (real PC 2026-11-24: with Dnscache disabled, names did not resolve): DnsQuery_W for the local
   computer name (answered by the DNS Client without traffic) and, if configured, for [network] dnsTestName. */
static int NetDnsQuery(const wchar_t *name)
{
    PDNS_RECORD r = NULL, p;
    DNS_STATUS st;
    wchar_t ip[24] = L"";
    DWORD t0 = GetTickCount();
    st = DnsQuery_W(name, DNS_TYPE_A, DNS_QUERY_STANDARD, NULL, &r, NULL);
    for (p = r; p && st == 0; p = p->pNext) {
        if (p->wType == DNS_TYPE_A) { const BYTE *b = (const BYTE *)&p->Data.A.IpAddress; _snwprintf_s(ip, _countof(ip), _TRUNCATE, L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]); break; }
    }
    if (r) DnsRecordListFree(r, DnsFreeRecordList);
    Log(L"net on: DNS query '%s' -> %s%s (status %ld, %lu ms)", name, st == 0 ? L"OK " : L"FAILED", ip, (long)st, GetTickCount() - t0);
    return st == 0;
}

static void NetDnsCheck(const NetCfg *c)
{
    wchar_t host[256];
    DWORD cch = _countof(host);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) { Log(L"net on: DNS Client (Dnscache) state %lu (4 = running)", SvcCurrent(scm, L"Dnscache")); CloseServiceHandle(scm); }
    if (GetComputerNameExW(ComputerNameDnsHostname, host, &cch)) NetDnsQuery(host);
    if (c->dnsName[0]) NetDnsQuery(c->dnsName);
}

/* ======================================================================================================== */
/* net on / off / toggle / status                                                                           */
/* ======================================================================================================== */

static HANDLE NetLock(void)
{
    HANDLE m = CreateMutexW(NULL, FALSE, NET_OPMUTEX);
    if (!m) return NULL;
    if (WaitForSingleObject(m, 0) == WAIT_TIMEOUT) { CloseHandle(m); return NULL; }
    return m;
}
static void NetUnlock(HANDLE m) { if (m) { ReleaseMutex(m); CloseHandle(m); } }

static int NetOn(BOOL quiet)
{
    NetCfg c;
    SC_HANDLE scm;
    wchar_t buf[1024], *tok, *ctx = NULL, failed[512] = L"", list[256];
    NetAdapter a[MAXAD];
    int n, i, leases = 0, rcode;
    DWORD t0 = GetTickCount(), deadline, rcInit, rcWait;
    HANDLE lock;

    NetLoadCfg(&c);
    if (!NetManual(&c)) {
        Log(L"net on: refused, network.mode='%s' (networking is not part of this image)", c.mode);
        if (!quiet) Message(L"Networking is not included in this MyPE build (network mode '%s').", c.mode[0] ? c.mode : L"full-off");
        return NRC_NOTMANUAL;
    }
    if (!(lock = NetLock())) { Log(L"net on: another network operation is running"); if (!quiet) Message(L"A network operation is already running. Please wait."); return NRC_BUSY; }
    NetSetState(L"turning-on");
    Log(L"net on: begin (services %s; wait %lu s)", c.on, c.wait);
    NetDevices(TRUE, FALSE);
    Wpeutil(L"EnableFirewall", 30000);
    /* InitializeNetwork first: it installs the NIC/WLAN devices (and binds the network components to them) */
    rcInit = Wpeutil(L"InitializeNetwork", 120000);
    if (NetInstallNics()) Sleep(3000);   /* let NDIS bind the new adapters */
    NetDevices(TRUE, FALSE);   /* NICs installed above / by InitializeNetwork, or disabled by an earlier "net off" */
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) Log(L"net on: OpenSCManager FAILED err=%lu", GetLastError());
    else {
        SvcSetStartList(scm, c.bootDis, SERVICE_DEMAND_START);
        wcsncpy_s(buf, _countof(buf), c.on, _TRUNCATE);
        for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) {
            BOOL missing;
            if (!SvcStart(scm, tok, &missing) && !missing) { if (failed[0]) wcscat_s(failed, _countof(failed), L", "); if (wcslen(failed) + wcslen(tok) < 480) wcscat_s(failed, _countof(failed), tok); }
        }
        Log(L"net on: NativeWifiP (Native Wifi filter, loaded by NDIS for WLAN adapters) state %lu", SvcCurrent(scm, L"NativeWifiP"));
    }
    rcWait = Wpeutil(L"WaitForNetwork", c.wait * 1000);
    n = NetAdapters(a, MAXAD);
    deadline = t0 + c.wait * 1000;
    while (n > 0 && NetIPv4(&leases, NULL, 0) >= 0 && !leases && (LONG)(deadline - GetTickCount()) > 0) { Sleep(1000); n = NetAdapters(a, MAXAD); }
    NetIPv4(&leases, list, _countof(list));
    if (scm) {
        wchar_t st[400] = L"";
        wcsncpy_s(buf, _countof(buf), c.on, _TRUNCATE); ctx = NULL;
        for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) {
            wchar_t one[64];
            _snwprintf_s(one, _countof(one), _TRUNCATE, L"%s%s=%lu", st[0] ? L" " : L"", tok, SvcCurrent(scm, tok));
            if (wcslen(st) + wcslen(one) < _countof(st)) wcscat_s(st, _countof(st), one);
        }
        Log(L"net on: service states (4=running, 0=absent): %s", st);
        CloseServiceHandle(scm);
    }
    for (i = 0; i < n; i++) Log(L"net on: adapter '%s' (%s) type=%lu up=%d IPv4=[%s] gateway=%s DHCP server=%s DNS servers=[%s]", a[i].name, a[i].desc, a[i].type, a[i].up, a[i].ipv4, a[i].gw, a[i].dhcp, a[i].dns);
    NetDnsCheck(&c);
    NetSetState(L"on");
    rcode = n == 0 ? NRC_NOADAPTER : (leases ? NRC_OK : NRC_NOLEASE);
    Log(L"net on: done in %lu ms: adapters=%d IPv4 leases=%d [%s] InitializeNetwork=%ld WaitForNetwork=%ld failed services=[%s] -> rc=%d",
        GetTickCount() - t0, n, leases, list, (long)rcInit, (long)rcWait, failed, rcode);
    NetUnlock(lock);
    if (!quiet) {
        if (n == 0)
            Message(L"Network is ON, but no network adapter was found.\n\nThe network card or Wi-Fi card probably needs a driver that is not in MyPE "
                    L"(it can be exported from the installed Windows, like the display driver).%s%s",
                    failed[0] ? L"\n\nServices that did not start: " : L"", failed);
        else if (!leases)
            Message(L"Network is ON.\n\nNo IP address yet (cable connected? For Wi-Fi: open Network and connect).%s%s",
                    failed[0] ? L"\n\nServices that did not start: " : L"", failed);
        else Message(L"Network is ON.\n\nIP address: %s", list);
    }
    return rcode;
}

static int NetOff(BOOL quiet, BOOL boot)
{
    NetCfg c;
    SC_HANDLE scm;
    wchar_t buf[1024], *tok, *ctx = NULL, running[256] = L"", list[256];
    int ips, leases = 0, phys, k, rcode;
    DWORD t0 = GetTickCount();
    HANDLE lock;

    NetLoadCfg(&c);
    if (!NetManual(&c)) {
        Log(L"net off%s: network.mode='%s' - nothing to switch off", boot ? L" /boot" : L"", c.mode);
        if (!quiet && !boot) Message(L"Networking is not included in this MyPE build (network mode '%s').", c.mode[0] ? c.mode : L"full-off");
        return NRC_NOTMANUAL;
    }
    if (!(lock = NetLock())) { Log(L"net off: another network operation is running"); if (!quiet) Message(L"A network operation is already running. Please wait."); return NRC_BUSY; }
    NetSetState(L"turning-off");
    Log(L"net off%s: begin", boot ? L" /boot" : L"");
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (scm && SvcCurrent(scm, L"WlanSvc") == SERVICE_RUNNING) WlanDisconnectAll();
    NetIPv4(&leases, NULL, 0);
    if (leases) { wchar_t cmd[MAX_PATH + 32], sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\\ipconfig.exe\" /release", sys); Log(L"net off: ipconfig /release -> rc=%ld", (long)RunWait(cmd, 20000, NULL)); }
    phys = NetDevices(FALSE, FALSE);
    if (scm) {
        wcsncpy_s(buf, _countof(buf), c.off, _TRUNCATE);
        for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) SvcStopTree(scm, tok, 0);
        SvcSetStartList(scm, c.bootDis, SERVICE_DISABLED);
        if (SvcCurrent(scm, L"Dnscache") == SERVICE_RUNNING) Log(L"net off: Dnscache (DNS Client) left running: it refuses SERVICE_STOP by design; without adapters / IP it sends nothing. its start type goes back to disabled (bootDisabled)");
    }
    /* verify */
    for (k = 0; k < 20 && (ips = NetIPv4(&leases, list, _countof(list))) > 0; k++) Sleep(500);
    ips = NetIPv4(&leases, list, _countof(list));
    if (scm) {
        wcsncpy_s(buf, _countof(buf), c.off, _TRUNCATE); ctx = NULL;
        for (tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(NULL, L", ", &ctx)) {
            DWORD s = SvcCurrent(scm, tok);
            if (s && s != SERVICE_STOPPED && InList(c.bootDis, tok)) {
                Log(L"net off: %s still running: it refuses stop by design; start type disabled, no adapter/IP so no traffic (accepted)", tok);
                continue;
            }
            if (s && s != SERVICE_STOPPED) { if (running[0]) wcscat_s(running, _countof(running), L", "); if (wcslen(running) + wcslen(tok) < 240) wcscat_s(running, _countof(running), tok); }
        }
        CloseServiceHandle(scm);
    }
    NetSetState(L"off");
    rcode = (ips == 0 && !running[0]) ? NRC_OK : NRC_NOTVERIFIED;
    Log(L"net off%s: done in %lu ms: physical NICs=%d IPv4 addresses=%d [%s] still running=[%s] -> %s", boot ? L" /boot" : L"",
        GetTickCount() - t0, phys, ips, list, running, rcode == NRC_OK ? L"OFF verified" : L"WARNING: OFF NOT verified");
    NetUnlock(lock);
    if (!quiet && !boot) {
        if (rcode == NRC_OK) Message(L"Network is OFF.");
        else Message(L"Network was switched off, but the check failed:\nIPv4 addresses: %s\nservices still running: %s\n\nSee X:\\MyPE\\logs\\mypeui.log.", list[0] ? list : L"none", running[0] ? running : L"none");
    }
    return rcode;
}

/* current state for tray/window: intent (registry) + reality (Dhcp service, IPv4 leases) */
static int NetState(wchar_t *ips, size_t cch)
{
    wchar_t s[32];
    int leases = 0;
    SC_HANDLE scm;
    DWORD dhcp = 0;
    NetGetState(s, _countof(s));
    NetIPv4(&leases, ips, cch);
    if (!_wcsicmp(s, L"turning-on") || !_wcsicmp(s, L"turning-off")) {
        /* only while an operation really runs: a MyPEUI killed during "net on/off" leaves the state behind
           (its mutex is then free or abandoned) -> judge by the real state below */
        HANDLE m = NetLock();
        if (!m) return !_wcsicmp(s, L"turning-on") ? NS_TURNING_ON : NS_TURNING_OFF;
        Log(L"net: stale state '%s' (no operation running) - reset to the real state (%s)", s, leases ? L"on" : L"off");
        NetSetState(leases ? L"on" : L"off");     /* while holding the lock: no operation can start in between */
        NetUnlock(m);
        s[0] = 0;
    }
    if (leases) return NS_CONNECTED;
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) { dhcp = SvcCurrent(scm, L"Dhcp"); CloseServiceHandle(scm); }
    if (!_wcsicmp(s, L"on") || dhcp == SERVICE_RUNNING) return NS_ON;
    return NS_OFF;
}

static int NetToggle(BOOL quiet)
{
    wchar_t ips[128];
    int st = NetState(ips, _countof(ips));
    Log(L"net toggle: state %s", s_stateName[st]);
    if (st == NS_ON || st == NS_CONNECTED || st == NS_TURNING_ON) return NetOff(quiet, FALSE);
    return NetOn(quiet);
}

int NetCommand(int argc, wchar_t **argv)
{
    BOOL quiet = FALSE, boot = FALSE;
    const wchar_t *verb = argc > 2 ? argv[2] : L"";
    int i;
    for (i = 3; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/quiet") || !_wcsicmp(argv[i], L"-quiet")) quiet = TRUE;
        else if (!_wcsicmp(argv[i], L"/boot") || !_wcsicmp(argv[i], L"-boot")) { boot = TRUE; quiet = TRUE; }
    }
    if (!_wcsicmp(verb, L"on")) return NetOn(quiet);
    if (!_wcsicmp(verb, L"off")) return NetOff(quiet, boot);
    if (!_wcsicmp(verb, L"toggle")) return NetToggle(quiet);
    if (!_wcsicmp(verb, L"status")) {
        wchar_t ips[256];
        NetCfg c;
        int st = NetState(ips, _countof(ips));
        NetLoadCfg(&c);
        Log(L"net status: mode=%s state=%s IPv4=[%s]", c.mode, s_stateName[st], ips);
        NetDevices(FALSE, TRUE);
        return st;
    }
    Log(L"net: unknown verb '%s'", verb);
    if (!quiet) Message(L"MyPEUI.exe net on|off|toggle|status [/quiet]");
    return 2;
}

/* starts "MyPEUI.exe <args>" (same exe), returns the process handle or NULL */
static HANDLE SpawnSelf(const wchar_t *args)
{
    wchar_t exe[MAX_PATH], line[MAX_PATH + 128];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"\"%s\" %s", exe, args);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { Log(L"net: start FAILED err=%lu: %s", GetLastError(), line); return NULL; }
    Log(L"net: started pid %lu: %s", pi.dwProcessId, args);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* ======================================================================================================== */
/* tray icon                                                                                                 */
/* ======================================================================================================== */

static void Px(DWORD *b, int sz, int x, int y, DWORD rgb, double cov)
{
    DWORD d;
    double sa, da, oa;
    int r, g, bl;
    if (x < 0 || y < 0 || x >= sz || y >= sz || cov <= 0) return;
    if (cov > 1) cov = 1;
    d = b[y * sz + x];
    sa = cov; da = (d >> 24) / 255.0; oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    r = (int)((((rgb >> 16) & 255) * sa + ((d >> 16) & 255) * da * (1 - sa)) / oa + 0.5);
    g = (int)((((rgb >> 8) & 255) * sa + ((d >> 8) & 255) * da * (1 - sa)) / oa + 0.5);
    bl = (int)(((rgb & 255) * sa + (d & 255) * da * (1 - sa)) / oa + 0.5);
    b[y * sz + x] = ((DWORD)(oa * 255 + 0.5) << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)bl;
}

static double Cl(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* signal bars + status dot (OFF: grey bars, red slash; ON: dim bars, amber dot; connected: white bars, green dot;
   turning on/off: dim bars, blue dot) */
static HICON MakeNetIcon(int state, int sz)
{
    BITMAPV5HEADER bh;
    DWORD *bits = NULL;
    HDC dc;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON ic;
    int i, x, y;
    double s = sz / 16.0, bw = 2.4 * s, gap = 1.2 * s, base = sz - 1.5 * s;
    DWORD bar = state == NS_CONNECTED ? 0xFFFFFF : (state == NS_OFF ? 0x9AA0A6 : 0xD8DCE0);
    double barA = state == NS_CONNECTED ? 1.0 : (state == NS_OFF ? 0.9 : 0.55);
    DWORD dot = state == NS_CONNECTED ? 0x3CC864 : (state == NS_ON ? 0xF0B429 : 0x4AA3FF);

    ZeroMemory(&bh, sizeof(bh));
    bh.bV5Size = sizeof(bh); bh.bV5Width = sz; bh.bV5Height = -sz; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS; bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return NULL;
    ZeroMemory(bits, (size_t)sz * sz * 4);
    for (i = 0; i < 4; i++) {
        double x0 = 1.0 * s + i * (bw + gap), x1 = x0 + bw, top = base - (0.28 + 0.2 * i) * (sz - 3 * s);
        for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
            double cx = Cl(x + 1 - x0) * Cl(x1 - x), cy = Cl(y + 1 - top) * Cl(base - y);
            Px(bits, sz, x, y, bar, cx * cy * barA);
        }
    }
    if (state == NS_OFF) {   /* red slash from bottom-left to top-right */
        double ax = 1.5 * s, ay = sz - 1.5 * s, bx = sz - 1.5 * s, by = 1.5 * s, t = 1.25 * s;
        double ex = bx - ax, ey = by - ay, len = sqrt(ex * ex + ey * ey);
        for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
            double px = x + 0.5, py = y + 0.5, u = ((px - ax) * ex + (py - ay) * ey) / (len * len), d;
            u = Cl(u);
            d = sqrt((ax + u * ex - px) * (ax + u * ex - px) + (ay + u * ey - py) * (ay + u * ey - py));
            Px(bits, sz, x, y, 0xE04848, Cl(t - d + 0.5));
        }
    } else {                 /* status dot, bottom right, with a dark ring */
        double cx = sz - 3.6 * s, cy = sz - 3.6 * s, r = 3.0 * s;
        for (y = 0; y < sz; y++) for (x = 0; x < sz; x++) {
            double d = sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            Px(bits, sz, x, y, 0x1E2230, Cl(r + 0.9 * s - d + 0.5));
            Px(bits, sz, x, y, dot, Cl(r - d + 0.5));
        }
    }
    mask = CreateBitmap(sz, sz, 1, 1, NULL);
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return ic;
}

static struct {
    HWND hwnd;
    UINT taskbarCreated;
    BOOL added;
    int state, iconState;
    HICON icon;
    HANDLE op;             /* running "net on/off" child */
    int opWant;            /* NS_ON / NS_OFF */
    wchar_t ips[256];
    DWORD lastAddTry;
} T;

static void TrayTip(NOTIFYICONDATAW *nd)
{
    const wchar_t *t;
    switch (T.state) {
    case NS_CONNECTED: t = L"Network: connected"; break;
    case NS_ON: t = L"Network: ON (no IP address)"; break;
    case NS_TURNING_ON: t = L"Network: turning on..."; break;
    case NS_TURNING_OFF: t = L"Network: turning off..."; break;
    default: t = L"Network: OFF"; break;
    }
    if (T.state == NS_CONNECTED && T.ips[0]) _snwprintf_s(nd->szTip, _countof(nd->szTip), _TRUNCATE, L"%s\n%s", t, T.ips);
    else _snwprintf_s(nd->szTip, _countof(nd->szTip), _TRUNCATE, L"%s\nClick: network window, right-click: menu", t);
}

static void TrayUpdate(BOOL force)
{
    NOTIFYICONDATAW nd;
    wchar_t title[320];
    int st = NetState(T.ips, _countof(T.ips));
    if (T.op && WaitForSingleObject(T.op, 0) == WAIT_TIMEOUT) st = T.opWant == NS_ON ? NS_TURNING_ON : NS_TURNING_OFF;
    _snwprintf_s(title, _countof(title), _TRUNCATE, L"MyPE network: %s %s", s_stateName[st], T.ips);
    SetWindowTextW(T.hwnd, title);
    if (st == T.state && T.added && !force) return;
    if (st != T.state) Log(L"tray-net: state %s -> %s [%s]", s_stateName[T.state], s_stateName[st], T.ips);
    T.state = st;
    if (!T.icon || T.iconState != st) {
        HICON old = T.icon;
        T.icon = MakeNetIcon(st, GetSystemMetrics(SM_CXSMICON));
        T.iconState = st;
        if (old) DestroyIcon(old);
    }
    ZeroMemory(&nd, sizeof(nd));
    nd.cbSize = sizeof(nd); nd.hWnd = T.hwnd; nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nd.uCallbackMessage = WM_TRAYCB; nd.hIcon = T.icon;
    TrayTip(&nd);
    if (T.added && Shell_NotifyIconW(NIM_MODIFY, &nd)) return;
    if (Shell_NotifyIconW(NIM_ADD, &nd)) { if (!T.added) Log(L"tray-net: icon added (state %s)", s_stateName[st]); T.added = TRUE; }
    else { if (T.added || GetTickCount() - T.lastAddTry > 30000) Log(L"tray-net: Shell_NotifyIcon NIM_ADD failed err=%lu (no tray yet?) - retrying", GetLastError()); T.added = FALSE; T.lastAddTry = GetTickCount(); }
}

static void TrayBalloon(const wchar_t *text)
{
    NOTIFYICONDATAW nd;
    if (!T.added) return;
    ZeroMemory(&nd, sizeof(nd));
    nd.cbSize = sizeof(nd); nd.hWnd = T.hwnd; nd.uID = 1; nd.uFlags = NIF_INFO; nd.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nd.szInfoTitle, _countof(nd.szInfoTitle), L"MyPE network", _TRUNCATE);
    wcsncpy_s(nd.szInfo, _countof(nd.szInfo), text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nd);
}

static void TrayStartOp(BOOL on)
{
    if (T.op && WaitForSingleObject(T.op, 0) == WAIT_TIMEOUT) { Log(L"tray-net: operation already running"); return; }
    if (T.op) { CloseHandle(T.op); T.op = NULL; }
    T.opWant = on ? NS_ON : NS_OFF;
    T.op = SpawnSelf(on ? L"net on /quiet" : L"net off /quiet");
    TrayUpdate(TRUE);
}

static void TrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    BOOL busy = T.op && WaitForSingleObject(T.op, 0) == WAIT_TIMEOUT, on = T.state == NS_ON || T.state == NS_CONNECTED;
    wchar_t head[300];
    int cmd;
    if (!m) return;
    _snwprintf_s(head, _countof(head), _TRUNCATE, L"Network: %s%s%s", T.state == NS_CONNECTED ? L"connected" : (on ? L"ON" : (busy ? L"busy" : L"OFF")),
                 T.ips[0] ? L"  " : L"", T.ips);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 1, head);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0) | (busy || on ? MF_GRAYED : 0), 10, L"Network ON");
    AppendMenuW(m, MF_STRING | (!on ? MF_CHECKED : 0) | (busy || !on ? MF_GRAYED : 0), 11, L"Network OFF");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 12, L"Wi-Fi...");
    AppendMenuW(m, MF_STRING, 13, L"Network window");
    GetCursorPos(&pt);
    SetForegroundWindow(T.hwnd);
    Log(L"tray-net: menu (state %s)", s_stateName[T.state]);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, T.hwnd, NULL);
    PostMessageW(T.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    Log(L"tray-net: menu command %d", cmd);
    if (cmd == 10) TrayStartOp(TRUE);
    else if (cmd == 11) TrayStartOp(FALSE);
    else if (cmd == 12 || cmd == 13) { HANDLE p = SpawnSelf(L"open network"); if (p) CloseHandle(p); }
}

static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == T.taskbarCreated && m) { Log(L"tray-net: TaskbarCreated - re-adding the icon"); T.added = FALSE; TrayUpdate(TRUE); return 0; }
    switch (m) {
    case WM_TIMER:
        if (T.op && WaitForSingleObject(T.op, 0) == WAIT_OBJECT_0) {
            DWORD rc = 1;
            GetExitCodeProcess(T.op, &rc);
            CloseHandle(T.op); T.op = NULL;
            Log(L"tray-net: net %s finished rc=%lu", T.opWant == NS_ON ? L"on" : L"off", rc);
            TrayUpdate(TRUE);
            if (T.opWant == NS_ON) TrayBalloon(rc == NRC_OK ? L"Network is ON." : (rc == NRC_NOADAPTER ? L"Network is ON, but no network adapter was found." :
                                                (rc == NRC_NOLEASE ? L"Network is ON, no IP address yet." : L"Network could not be switched on (see mypeui.log).")));
            else TrayBalloon(rc == NRC_OK ? L"Network is OFF." : L"Network switched off, but the check failed (see mypeui.log).");
        } else TrayUpdate(FALSE);
        return 0;
    case WM_TRAYCB:
        if (LOWORD(lp) == WM_LBUTTONUP) { HANDLE p = SpawnSelf(L"open network"); if (p) CloseHandle(p); }
        else if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_COMMAND:   /* tests: 10 = ON, 11 = OFF, 12 = open window, 14 = show the menu */
        if (LOWORD(wp) == 10) TrayStartOp(TRUE);
        else if (LOWORD(wp) == 11) TrayStartOp(FALSE);
        else if (LOWORD(wp) == 12) { HANDLE p = SpawnSelf(L"open network"); if (p) CloseHandle(p); }
        else if (LOWORD(wp) == 14) TrayMenu();
        return 0;
    case WM_CLOSE:
        return 0;   /* the tray icon has no exit (spec 14) */
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int NetTray(void)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, TRAY_MUTEX);
    WNDCLASSEXW wc;
    MSG msg;
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { Log(L"tray-net: already running"); CloseHandle(mutex); return 0; }
    ZeroMemory(&T, sizeof(T));
    T.state = NS_OFF; T.iconState = -1;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = TrayProc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = TRAY_CLASS;
    if (!RegisterClassExW(&wc)) { Log(L"tray-net: RegisterClass failed err=%lu", GetLastError()); return 1; }
    T.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    /* hidden top-level window (not HWND_MESSAGE: TaskbarCreated is broadcast to top-level windows only) */
    T.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, TRAY_CLASS, L"MyPE network", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!T.hwnd) { Log(L"tray-net: CreateWindow failed err=%lu", GetLastError()); return 1; }
    Log(L"tray-net: started (hwnd %p)", (void *)T.hwnd);
    TrayUpdate(TRUE);
    SetTimer(T.hwnd, 1, 2000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}

/* ======================================================================================================== */
/* "Network" window                                                                                          */
/* ======================================================================================================== */

enum {
    IDC_N_STATUS = 401, IDC_N_TOGGLE, IDC_N_ADAPTERS, IDC_N_WIFIHDR, IDC_N_LIST, IDC_N_PWLABEL, IDC_N_PW, IDC_N_CONNECT,
    IDC_N_DISCONNECT, IDC_N_REFRESH, IDC_N_CLOSE, IDC_N_WIFIMSG, IDC_N_WIFISTATUS
};

typedef struct { DOT11_SSID ssid; DOT11_BSS_TYPE bss; DOT11_AUTH_ALGORITHM auth; DOT11_CIPHER_ALGORITHM cipher; BOOL secured, connected; ULONG quality; wchar_t name[72]; } WNet;
typedef struct { DWORD code; DWORD reason; wchar_t ssid[72]; } WEvent;

static struct {
    HWND hwnd, list;
    HINSTANCE hi;
    HFONT font, fontBold;
    double s;
    HANDLE op;
    int opWant, state;
    HANDLE wh;            /* WLAN client handle */
    GUID ifGuid;
    BOOL haveIf;
    int nIf;
    wchar_t ifDesc[260];
    WNet nets[MAXNETS];
    int nNets;
    DWORD lastList, lastScan;
    wchar_t wifiMsg[300];
} N;

static HWND NCtl(int id) { return GetDlgItem(N.hwnd, id); }
static int NS(double v) { return (int)(v * N.s + 0.5); }

static void SsidText(const DOT11_SSID *s, wchar_t *out, size_t cch)
{
    int n;
    out[0] = 0;
    if (!s->uSSIDLength) return;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s->ucSSID, (int)s->uSSIDLength, out, (int)cch - 1);
    if (n <= 0) n = MultiByteToWideChar(1252, 0, (const char *)s->ucSSID, (int)s->uSSIDLength, out, (int)cch - 1);
    out[n > 0 ? n : 0] = 0;
}

static const wchar_t *AuthText(const WNet *w)
{
    if (!w->secured) return L"Open";
    switch ((int)w->auth) {
    case DOT11_AUTH_ALGO_RSNA_PSK: return w->cipher == DOT11_CIPHER_ALGO_TKIP ? L"WPA2-Personal (TKIP)" : L"WPA2-Personal";
    case DOT11_AUTH_ALGO_WPA3_SAE: return L"WPA3-Personal";
    case DOT11_AUTH_ALGO_WPA_PSK: return L"WPA-Personal (not supported)";
    case DOT11_AUTH_ALGO_RSNA: case DOT11_AUTH_ALGO_WPA: return L"Enterprise 802.1X (not supported)";
    case DOT11_AUTH_ALGO_80211_SHARED_KEY: case DOT11_AUTH_ALGO_80211_OPEN: return L"WEP (not supported)";
    default: return L"other (not supported)";
    }
}

static void NSetText(int id, const wchar_t *t) { wchar_t cur[600]; GetWindowTextW(NCtl(id), cur, _countof(cur)); if (wcscmp(cur, t)) SetWindowTextW(NCtl(id), t); }

static VOID WINAPI WlanCb(PWLAN_NOTIFICATION_DATA d, PVOID ctx)
{
    WEvent *e;
    (void)ctx;
    if (!d || d->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM || !N.hwnd) return;
    e = (WEvent *)calloc(1, sizeof(WEvent));
    if (!e) return;
    e->code = d->NotificationCode;
    if ((d->NotificationCode == wlan_notification_acm_connection_complete || d->NotificationCode == wlan_notification_acm_connection_attempt_fail ||
         d->NotificationCode == wlan_notification_acm_disconnected) && d->pData && d->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA)) {
        PWLAN_CONNECTION_NOTIFICATION_DATA c = (PWLAN_CONNECTION_NOTIFICATION_DATA)d->pData;
        e->reason = c->wlanReasonCode;
        SsidText(&c->dot11Ssid, e->ssid, _countof(e->ssid));
    }
    if (!PostMessageW(N.hwnd, WM_APP_WLAN, 0, (LPARAM)e)) free(e);
}

static void WifiClose(void)
{
    if (N.wh) { W.Close(N.wh, NULL); N.wh = NULL; Log(L"network: Wi-Fi handle closed"); }
    N.haveIf = FALSE; N.nIf = 0; N.nNets = 0;
    ListView_DeleteAllItems(N.list);
}

/* opens the WLAN handle (WlanSvc must run) and picks the interface; sets N.wifiMsg */
static void WifiOpen(void)
{
    DWORD ver = 0, rc;
    PWLAN_INTERFACE_INFO_LIST il = NULL;
    if (!N.wh) {
        if (!WlanLoad()) { wcsncpy_s(N.wifiMsg, _countof(N.wifiMsg), L"Wi-Fi is not available: this MyPE image has no WLAN components.", _TRUNCATE); return; }
        rc = W.Open(2, NULL, &ver, &N.wh);
        if (rc != ERROR_SUCCESS) {
            N.wh = NULL;
            _snwprintf_s(N.wifiMsg, _countof(N.wifiMsg), _TRUNCATE, L"Wi-Fi service (WlanSvc) is not available (error %lu).", rc);
            Log(L"network: WlanOpenHandle rc=%lu", rc);
            return;
        }
        if (W.Notify) { rc = W.Notify(N.wh, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, WlanCb, NULL, NULL, NULL); Log(L"network: WlanOpenHandle ok (version %lu), notifications rc=%lu", ver, rc); }
    }
    rc = W.Enum(N.wh, NULL, &il);
    N.haveIf = FALSE; N.nIf = 0;
    if (rc == ERROR_SUCCESS && il) {
        DWORD i, pick = 0;
        N.nIf = (int)il->dwNumberOfItems;
        for (i = 0; i < il->dwNumberOfItems; i++) if (il->InterfaceInfo[i].isState == wlan_interface_state_connected) pick = i;
        if (il->dwNumberOfItems) {
            N.ifGuid = il->InterfaceInfo[pick].InterfaceGuid; N.haveIf = TRUE;
            wcsncpy_s(N.ifDesc, _countof(N.ifDesc), il->InterfaceInfo[pick].strInterfaceDescription, _TRUNCATE);
        }
        W.Free(il);
    } else Log(L"network: WlanEnumInterfaces rc=%lu", rc);
    if (!N.haveIf) wcsncpy_s(N.wifiMsg, _countof(N.wifiMsg), L"No Wi-Fi adapter (WLAN interface) found. If this PC has Wi-Fi, its driver is probably not in MyPE.", _TRUNCATE);
    else _snwprintf_s(N.wifiMsg, _countof(N.wifiMsg), _TRUNCATE, L"Wi-Fi adapter: %s", N.ifDesc);
}

static void WifiList(void)
{
    PWLAN_AVAILABLE_NETWORK_LIST nl = NULL;
    DWORD rc, i;
    int k, sel = ListView_GetNextItem(N.list, -1, LVNI_SELECTED);
    wchar_t selName[72] = L"";
    if (sel >= 0 && sel < N.nNets) wcsncpy_s(selName, _countof(selName), N.nets[sel].name, _TRUNCATE);
    N.lastList = GetTickCount();
    if (!N.wh || !N.haveIf) return;
    rc = W.List(N.wh, &N.ifGuid, 0, NULL, &nl);
    if (rc != ERROR_SUCCESS || !nl) { Log(L"network: WlanGetAvailableNetworkList rc=%lu", rc); return; }
    N.nNets = 0;
    for (i = 0; i < nl->dwNumberOfItems && N.nNets < MAXNETS; i++) {
        PWLAN_AVAILABLE_NETWORK a = &nl->Network[i];
        wchar_t name[72];
        int dup = -1;
        SsidText(&a->dot11Ssid, name, _countof(name));
        if (!name[0]) continue;                                   /* hidden networks are not listed */
        for (k = 0; k < N.nNets; k++) if (!wcscmp(N.nets[k].name, name)) dup = k;
        if (dup >= 0) { if (a->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) N.nets[dup].connected = TRUE; if (a->wlanSignalQuality > N.nets[dup].quality) N.nets[dup].quality = a->wlanSignalQuality; continue; }
        N.nets[N.nNets].ssid = a->dot11Ssid; N.nets[N.nNets].bss = a->dot11BssType; N.nets[N.nNets].auth = a->dot11DefaultAuthAlgorithm;
        N.nets[N.nNets].cipher = a->dot11DefaultCipherAlgorithm; N.nets[N.nNets].secured = a->bSecurityEnabled;
        N.nets[N.nNets].connected = (a->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0; N.nets[N.nNets].quality = a->wlanSignalQuality;
        wcsncpy_s(N.nets[N.nNets].name, _countof(N.nets[N.nNets].name), name, _TRUNCATE);
        N.nNets++;
    }
    W.Free(nl);
    SendMessageW(N.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(N.list);
    for (k = 0; k < N.nNets; k++) {
        LVITEMW it;
        wchar_t q[16];
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = k; it.pszText = N.nets[k].name; it.lParam = k;
        ListView_InsertItem(N.list, &it);
        _snwprintf_s(q, _countof(q), _TRUNCATE, L"%lu%%", N.nets[k].quality);
        ListView_SetItemText(N.list, k, 1, q);
        ListView_SetItemText(N.list, k, 2, (LPWSTR)AuthText(&N.nets[k]));
        ListView_SetItemText(N.list, k, 3, N.nets[k].connected ? L"Connected" : L"");
        if (selName[0] && !wcscmp(selName, N.nets[k].name)) ListView_SetItemState(N.list, k, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(N.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(N.list, NULL, TRUE);
    Log(L"network: Wi-Fi list: %d networks", N.nNets);
}

static void XmlAppend(wchar_t *dst, size_t cch, const wchar_t *s)
{
    for (; *s; s++) {
        const wchar_t *r = NULL;
        wchar_t one[2] = { *s, 0 };
        switch (*s) { case L'&': r = L"&amp;"; break; case L'<': r = L"&lt;"; break; case L'>': r = L"&gt;"; break; case L'"': r = L"&quot;"; break; case L'\'': r = L"&apos;"; break; }
        wcscat_s(dst, cch, r ? r : one);
    }
}

static void WifiConnect(void)
{
    int sel = ListView_GetNextItem(N.list, -1, LVNI_SELECTED);
    WNet *w;
    wchar_t pw[80], xml[4096], hex[80], st[200];
    const wchar_t *auth = NULL, *enc = NULL;
    WLAN_CONNECTION_PARAMETERS cp;
    DWORD rc, i;
    size_t plen;
    if (!N.wh || !N.haveIf) return;
    if (sel < 0 || sel >= N.nNets) { NSetText(IDC_N_WIFISTATUS, L"Select a network in the list first."); return; }
    w = &N.nets[sel];
    if (!w->secured) { auth = L"open"; enc = L"none"; }
    else if (w->auth == DOT11_AUTH_ALGO_RSNA_PSK) { auth = L"WPA2PSK"; enc = w->cipher == DOT11_CIPHER_ALGO_TKIP ? L"TKIP" : L"AES"; }
    else if ((int)w->auth == (int)DOT11_AUTH_ALGO_WPA3_SAE) { auth = L"WPA3SAE"; enc = L"AES"; }
    else { _snwprintf_s(st, _countof(st), _TRUNCATE, L"'%s' uses %s, which MyPE cannot connect to.", w->name, AuthText(w)); NSetText(IDC_N_WIFISTATUS, st); return; }
    GetWindowTextW(NCtl(IDC_N_PW), pw, _countof(pw));
    plen = wcslen(pw);
    if (w->secured) {
        BOOL hexKey = plen == 64;
        for (i = 0; hexKey && i < 64; i++) hexKey = iswxdigit(pw[i]) != 0;
        if (!hexKey && (plen < 8 || plen > 63)) { NSetText(IDC_N_WIFISTATUS, L"The Wi-Fi password must have 8 to 63 characters."); SecureZeroMemory(pw, sizeof(pw)); return; }
    }
    hex[0] = 0;
    for (i = 0; i < w->ssid.uSSIDLength && i < 32; i++) { wchar_t b[4]; _snwprintf_s(b, _countof(b), _TRUNCATE, L"%02X", w->ssid.ucSSID[i]); wcscat_s(hex, _countof(hex), b); }
    /* temporary profile, in memory only (wlan_connection_mode_temporary_profile is never stored) */
    wcscpy_s(xml, _countof(xml), L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>MyPE-temporary</name><SSIDConfig><SSID><hex>");
    wcscat_s(xml, _countof(xml), hex);
    wcscat_s(xml, _countof(xml), L"</hex></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>");
    wcscat_s(xml, _countof(xml), auth);
    wcscat_s(xml, _countof(xml), L"</authentication><encryption>");
    wcscat_s(xml, _countof(xml), enc);
    wcscat_s(xml, _countof(xml), L"</encryption><useOneX>false</useOneX></authEncryption>");
    if (w->secured) {
        wcscat_s(xml, _countof(xml), plen == 64 ? L"<sharedKey><keyType>networkKey</keyType><protected>false</protected><keyMaterial>" : L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>");
        XmlAppend(xml, _countof(xml), pw);
        wcscat_s(xml, _countof(xml), L"</keyMaterial></sharedKey>");
    }
    wcscat_s(xml, _countof(xml), L"</security></MSM></WLANProfile>");
    ZeroMemory(&cp, sizeof(cp));
    cp.wlanConnectionMode = wlan_connection_mode_temporary_profile;
    cp.strProfile = xml;
    cp.pDot11Ssid = &w->ssid;
    cp.dot11BssType = w->bss;
    rc = W.Connect(N.wh, &N.ifGuid, &cp, NULL);
    SecureZeroMemory(xml, sizeof(xml));
    SecureZeroMemory(pw, sizeof(pw));
    Log(L"network: Wi-Fi connect '%s' (%s/%s, temporary profile) -> rc=%lu", w->name, auth, enc, rc);
    if (rc == ERROR_SUCCESS) { _snwprintf_s(st, _countof(st), _TRUNCATE, L"Connecting to '%s'...", w->name); SetWindowTextW(NCtl(IDC_N_PW), L""); }
    else _snwprintf_s(st, _countof(st), _TRUNCATE, L"Could not start the connection to '%s' (error %lu).", w->name, rc);
    NSetText(IDC_N_WIFISTATUS, st);
}

static void NRefresh(BOOL forceList)
{
    wchar_t t[1200], line[400];
    NetAdapter a[MAXAD];
    int n, i;
    BOOL busy = N.op && WaitForSingleObject(N.op, 0) == WAIT_TIMEOUT, on;
    wchar_t ips[256];
    if (N.op && !busy) { DWORD rc = 1; GetExitCodeProcess(N.op, &rc); Log(L"network: net %s finished rc=%lu", N.opWant == NS_ON ? L"on" : L"off", rc); CloseHandle(N.op); N.op = NULL; forceList = TRUE; }
    N.state = NetState(ips, _countof(ips));
    if (busy) N.state = N.opWant == NS_ON ? NS_TURNING_ON : NS_TURNING_OFF;
    on = N.state == NS_ON || N.state == NS_CONNECTED;
    switch (N.state) {
    case NS_CONNECTED: NSetText(IDC_N_STATUS, L"Network is ON (connected)"); break;
    case NS_ON: NSetText(IDC_N_STATUS, L"Network is ON (no IP address)"); break;
    case NS_TURNING_ON: NSetText(IDC_N_STATUS, L"Turning the network on... (up to a minute)"); break;
    case NS_TURNING_OFF: NSetText(IDC_N_STATUS, L"Turning the network off..."); break;
    default: NSetText(IDC_N_STATUS, L"Network is OFF"); break;
    }
    NSetText(IDC_N_TOGGLE, on || N.state == NS_TURNING_OFF ? L"Turn network OFF" : L"Turn network ON");
    EnableWindow(NCtl(IDC_N_TOGGLE), !busy && N.state != NS_TURNING_ON && N.state != NS_TURNING_OFF);
    n = NetAdapters(a, MAXAD);
    t[0] = 0;
    for (i = 0; i < n; i++) {
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%s%s (%s): %s%s%s%s%s%s%s", t[0] ? L"\r\n" : L"", a[i].name, a[i].desc,
                     a[i].ipv4[0] ? a[i].ipv4 : (a[i].up ? L"no IPv4 address" : L"not connected"), a[i].gw[0] ? L", gateway " : L"", a[i].gw,
                     a[i].dhcp[0] ? L", DHCP " : L"", a[i].dhcp, a[i].dns[0] ? L", DNS " : L"", a[i].dns);
        if (wcslen(t) + wcslen(line) < _countof(t)) wcscat_s(t, _countof(t), line);
    }
    if (!n) wcscpy_s(t, _countof(t), on ? L"No network adapter found (its driver may be missing in MyPE)." : L"Adapters appear when the network is ON.");
    NSetText(IDC_N_ADAPTERS, t);
    /* Wi-Fi */
    if (!on) {
        if (N.wh) WifiClose();
        wcsncpy_s(N.wifiMsg, _countof(N.wifiMsg), busy || N.state == NS_TURNING_ON ? L"Wi-Fi: waiting for the network..." : L"Turn the network on to use Wi-Fi.", _TRUNCATE);
    } else {
        int had = N.haveIf;
        if (!N.wh || forceList || GetTickCount() - N.lastList > 10000) WifiOpen();
        if (N.haveIf && (!had || forceList || GetTickCount() - N.lastList > 8000)) {
            if (!had || GetTickCount() - N.lastScan > 30000) { DWORD rc = W.Scan(N.wh, &N.ifGuid, NULL, NULL, NULL); N.lastScan = GetTickCount(); Log(L"network: WlanScan rc=%lu", rc); }
            WifiList();
        }
        if (!N.haveIf && N.nNets) { N.nNets = 0; ListView_DeleteAllItems(N.list); }
    }
    NSetText(IDC_N_WIFIMSG, N.wifiMsg);
    EnableWindow(N.list, on && N.haveIf);
    EnableWindow(NCtl(IDC_N_PW), on && N.haveIf);
    EnableWindow(NCtl(IDC_N_CONNECT), on && N.haveIf);
    EnableWindow(NCtl(IDC_N_DISCONNECT), on && N.haveIf);
    EnableWindow(NCtl(IDC_N_REFRESH), on && N.haveIf);
}

static void NLayout(void)
{
    RECT rc;
    int W_, H, m = NS(14), y, bw = NS(150), bh = NS(30), lh = NS(20);
    GetClientRect(N.hwnd, &rc);
    W_ = rc.right; H = rc.bottom;
    y = m;
    MoveWindow(NCtl(IDC_N_STATUS), m, y + NS(5), W_ - 3 * m - bw, lh + NS(4), TRUE);
    MoveWindow(NCtl(IDC_N_TOGGLE), W_ - m - bw, y, bw, bh, TRUE);
    y += bh + NS(10);
    MoveWindow(NCtl(IDC_N_ADAPTERS), m, y, W_ - 2 * m, NS(64), TRUE);
    y += NS(70);
    MoveWindow(NCtl(IDC_N_WIFIHDR), m, y, W_ - 2 * m, lh, TRUE);
    y += lh + NS(2);
    MoveWindow(NCtl(IDC_N_WIFIMSG), m, y, W_ - 2 * m, NS(36), TRUE);
    y += NS(40);
    {
        int listH = H - y - m - 3 * bh - NS(40);
        if (listH < NS(80)) listH = NS(80);
        MoveWindow(N.list, m, y, W_ - 2 * m, listH, TRUE);
        ListView_SetColumnWidth(N.list, 0, (W_ - 2 * m) - NS(300) - GetSystemMetrics(SM_CXVSCROLL));
        ListView_SetColumnWidth(N.list, 1, NS(60));
        ListView_SetColumnWidth(N.list, 2, NS(150));
        ListView_SetColumnWidth(N.list, 3, NS(86));
        y += listH + NS(8);
    }
    MoveWindow(NCtl(IDC_N_PWLABEL), m, y + NS(6), NS(80), lh, TRUE);
    MoveWindow(NCtl(IDC_N_PW), m + NS(84), y, W_ - 2 * m - NS(84) - 2 * (NS(100) + NS(8)), bh - NS(4), TRUE);
    MoveWindow(NCtl(IDC_N_CONNECT), W_ - m - 2 * NS(100) - NS(8), y - NS(1), NS(100), bh, TRUE);
    MoveWindow(NCtl(IDC_N_DISCONNECT), W_ - m - NS(100), y - NS(1), NS(100), bh, TRUE);
    y += bh + NS(6);
    MoveWindow(NCtl(IDC_N_WIFISTATUS), m, y, W_ - 2 * m, NS(36), TRUE);
    MoveWindow(NCtl(IDC_N_REFRESH), m, H - m - bh, NS(100), bh, TRUE);
    MoveWindow(NCtl(IDC_N_CLOSE), W_ - m - NS(100), H - m - bh, NS(100), bh, TRUE);
}

static LRESULT CALLBACK NetProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_N_TOGGLE: {
            BOOL on = N.state == NS_ON || N.state == NS_CONNECTED;
            if (N.op && WaitForSingleObject(N.op, 0) == WAIT_TIMEOUT) break;
            if (N.op) { CloseHandle(N.op); N.op = NULL; }
            N.opWant = on ? NS_OFF : NS_ON;
            if (on && N.wh) WifiClose();   /* release the WLAN handle before WlanSvc is stopped */
            N.op = SpawnSelf(on ? L"net off /quiet" : L"net on /quiet");
            NRefresh(FALSE);
            break;
        }
        case IDC_N_CONNECT: WifiConnect(); break;
        case IDC_N_DISCONNECT:
            if (N.wh && N.haveIf) { DWORD rc = W.Disconnect(N.wh, &N.ifGuid, NULL); Log(L"network: Wi-Fi disconnect rc=%lu", rc); NSetText(IDC_N_WIFISTATUS, rc ? L"Disconnect failed." : L"Disconnected."); }
            break;
        case IDC_N_REFRESH:
            if (N.wh && N.haveIf) { DWORD rc = W.Scan(N.wh, &N.ifGuid, NULL, NULL, NULL); N.lastScan = GetTickCount(); Log(L"network: WlanScan (Refresh) rc=%lu", rc); }
            NRefresh(TRUE);
            break;
        case IDC_N_CLOSE: case IDCANCEL: DestroyWindow(h); break;
        }
        return 0;
    case WM_TIMER:
        NRefresh(FALSE);
        return 0;
    case WM_APP_WLAN: {
        WEvent *e = (WEvent *)lp;
        wchar_t st[400], why[256] = L"";
        if (!e) return 0;
        if (e->code == wlan_notification_acm_scan_complete || e->code == wlan_notification_acm_scan_fail || e->code == wlan_notification_acm_network_available) WifiList();
        else if (e->code == wlan_notification_acm_connection_complete || e->code == wlan_notification_acm_connection_attempt_fail) {
            if (e->reason == WLAN_REASON_CODE_SUCCESS) _snwprintf_s(st, _countof(st), _TRUNCATE, L"Connected to '%s'. The IP address comes from DHCP.", e->ssid);
            else {
                if (W.Reason) W.Reason(e->reason, _countof(why), why, NULL);
                _snwprintf_s(st, _countof(st), _TRUNCATE, L"Could not connect to '%s': %s (reason %lu)", e->ssid, why[0] ? why : L"unknown", e->reason);
            }
            Log(L"network: Wi-Fi event %lu '%s' reason %lu %s", e->code, e->ssid, e->reason, why);
            NSetText(IDC_N_WIFISTATUS, st);
            WifiList();
        } else if (e->code == wlan_notification_acm_disconnected) {
            _snwprintf_s(st, _countof(st), _TRUNCATE, L"Disconnected from '%s'.", e->ssid);
            Log(L"network: Wi-Fi disconnected '%s' reason %lu", e->ssid, e->reason);
            NSetText(IDC_N_WIFISTATUS, st);
            WifiList();
        } else if (e->code == wlan_notification_acm_interface_arrival || e->code == wlan_notification_acm_interface_removal) {
            Log(L"network: WLAN interface %s", e->code == wlan_notification_acm_interface_arrival ? L"arrived" : L"removed");
            NRefresh(TRUE);
        }
        free(e);
        return 0;
    }
    case WM_SIZE:
        NLayout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = NS(520); mm->ptMinTrackSize.y = NS(520);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        N.s = HIWORD(wp) / 96.0;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (N.wh) WifiClose();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static BOOL CALLBACK NSetFont(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

int NetWindow(const wchar_t *uri)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, NETW_MUTEX);
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MONITORINFO mi;
    POINT pt = { 0, 0 };
    RECT rc;
    MSG msg;
    LVCOLUMNW col;
    NetCfg c;
    static const struct { const wchar_t *cls, *text; DWORD style; int id; } ctl[] = {
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_N_STATUS },
        { L"BUTTON", L"Turn network ON", BS_PUSHBUTTON | WS_TABSTOP, IDC_N_TOGGLE },
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_N_ADAPTERS },
        { L"STATIC", L"Wi-Fi", SS_LEFT, IDC_N_WIFIHDR },
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_N_WIFIMSG },
        { L"STATIC", L"Password:", SS_LEFT, IDC_N_PWLABEL },
        { L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, IDC_N_PW },
        { L"BUTTON", L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_N_CONNECT },
        { L"BUTTON", L"Disconnect", BS_PUSHBUTTON | WS_TABSTOP, IDC_N_DISCONNECT },
        { L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDC_N_WIFISTATUS },
        { L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, IDC_N_REFRESH },
        { L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_N_CLOSE },
    };
    size_t i;

    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(NETW_CLASS, NULL);
        Log(L"network: window already open (hwnd %p) - brought to front", (void *)other);
        if (other) { ShowWindow(other, SW_RESTORE); ForceForeground(other); }
        CloseHandle(mutex);
        return 0;
    }
    NetLoadCfg(&c);
    if (!NetManual(&c)) {
        Log(L"network: window refused, network.mode='%s'", c.mode);
        Message(L"Networking is not included in this MyPE build (network mode '%s').", c.mode[0] ? c.mode : L"full-off");
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    ZeroMemory(&N, sizeof(N));
    N.hi = GetModuleHandleW(NULL);
    icc.dwSize = sizeof(icc); icc.dwICC = ICC_LISTVIEW_CLASSES;
    if (!InitCommonControlsEx(&icc)) Log(L"network: InitCommonControlsEx failed err=%lu", GetLastError());
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = NetProc; wc.hInstance = N.hi; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = NETW_CLASS;
    wc.hIcon = MakeNetIcon(NS_CONNECTED, GetSystemMetrics(SM_CXICON));
    if (!RegisterClassExW(&wc)) { Log(L"network: RegisterClass failed err=%lu", GetLastError()); return 1; }
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    N.hwnd = CreateWindowExW(WS_EX_APPWINDOW, NETW_CLASS, L"Network", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, mi.rcWork.left + 60, mi.rcWork.top + 60, 600, 640, NULL, NULL, N.hi, NULL);
    if (!N.hwnd) { Log(L"network: CreateWindow failed err=%lu", GetLastError()); return 1; }
    N.s = WindowDpi(N.hwnd) / 96.0;
    SetWindowPos(N.hwnd, NULL, 0, 0, NS(600), NS(640), SWP_NOMOVE | SWP_NOZORDER);
    N.font = CreateFontW(-NS(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    N.fontBold = CreateFontW(-NS(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (i = 0; i < _countof(ctl); i++)
        CreateWindowExW(0, ctl[i].cls, ctl[i].text, WS_CHILD | WS_VISIBLE | ctl[i].style, 0, 0, 10, 10, N.hwnd, (HMENU)(INT_PTR)ctl[i].id, N.hi, NULL);
    N.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 10, 10, N.hwnd, (HMENU)(INT_PTR)IDC_N_LIST, N.hi, NULL);
    if (!N.list) Log(L"network: ListView not created err=%lu", GetLastError());
    ListView_SetExtendedListViewStyle(N.list, LVS_EX_FULLROWSELECT);
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 200; col.pszText = L"Network (SSID)"; ListView_InsertColumn(N.list, 0, &col);
    col.cx = 60; col.pszText = L"Signal"; ListView_InsertColumn(N.list, 1, &col);
    col.cx = 150; col.pszText = L"Security"; ListView_InsertColumn(N.list, 2, &col);
    col.cx = 80; col.pszText = L"State"; ListView_InsertColumn(N.list, 3, &col);
    EnumChildWindows(N.hwnd, NSetFont, (LPARAM)N.font);
    SendMessageW(NCtl(IDC_N_STATUS), WM_SETFONT, (WPARAM)N.fontBold, TRUE);
    SendMessageW(NCtl(IDC_N_WIFIHDR), WM_SETFONT, (WPARAM)N.fontBold, TRUE);
    NLayout();
    NRefresh(TRUE);
    GetWindowRect(N.hwnd, &rc);
    SetWindowPos(N.hwnd, NULL, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (rc.right - rc.left)) / 2,
                 mi.rcWork.top + max(0, ((mi.rcWork.bottom - mi.rcWork.top) - (rc.bottom - rc.top)) / 2), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(N.hwnd, SW_SHOW);
    ForceForeground(N.hwnd);
    SetTimer(N.hwnd, 1, 2000, NULL);
    GetWindowRect(N.hwnd, &rc);
    Log(L"network: window open (uri=%s) dpi-scale=%.2f rect=%ld,%ld,%ldx%ld state=%s wifi='%s'", uri ? uri : L"", N.s, rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top, s_stateName[N.state], N.wifiMsg);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(N.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    N.hwnd = NULL;
    if (N.op) CloseHandle(N.op);
    if (N.font) DeleteObject(N.font);
    if (N.fontBold) DeleteObject(N.fontBold);
    Log(L"network: window closed");
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
