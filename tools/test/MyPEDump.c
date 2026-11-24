/*
 * MyPEDump.exe - writes a minidump of a running process (own code, uses dbghelp.dll from Windows). Test images only.
 *   MyPEDump.exe <pid> <dumpfile> [full]
 * Default: threads + handles + unloaded modules (enough for stacks / hang analysis with cdb); "full" adds all memory.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#pragma comment(lib, "dbghelp.lib")

int wmain(int argc, wchar_t **argv)
{
    DWORD pid;
    HANDLE proc, file;
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                                         MiniDumpWithProcessThreadData | MiniDumpWithDataSegs);
    if (argc < 3) { fwprintf(stderr, L"usage: MyPEDump <pid> <dumpfile> [full]\n"); return 2; }
    pid = (DWORD)_wtoi(argv[1]);
    if (argc > 3 && _wcsicmp(argv[3], L"full") == 0) type = (MINIDUMP_TYPE)(type | MiniDumpWithFullMemory);
    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!proc) { fwprintf(stderr, L"OpenProcess %lu failed %lu\n", pid, GetLastError()); return 1; }
    file = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"CreateFile failed %lu\n", GetLastError()); CloseHandle(proc); return 1; }
    if (!MiniDumpWriteDump(proc, pid, file, type, NULL, NULL, NULL)) {
        fwprintf(stderr, L"MiniDumpWriteDump failed 0x%08lx\n", (unsigned long)HRESULT_FROM_WIN32(GetLastError()));
        CloseHandle(file); CloseHandle(proc); return 1;
    }
    CloseHandle(file); CloseHandle(proc);
    wprintf(L"dump written: %s\n", argv[2]);
    return 0;
}
