# Building WinPE-Shell

## Requirements
- Windows 10 or 11, x64 (the build machine; the binaries target WinPE x64).
- **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the workload *Desktop development with C++*:
  MSVC v143 x64/x86 build tools and the **Windows 11 SDK (10.0.26100.0)**.
- PowerShell 5.1 or 7. No other tools: no NuGet, vcpkg, CMake or network access during the build.

Installing Build Tools from the command line (official Microsoft bootstrapper `vs_BuildTools.exe`):
```
vs_BuildTools.exe --passive --wait --add Microsoft.VisualStudio.Workload.VCTools ^
    --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100
```

## Build
```powershell
.\build.ps1                        # shell + UI utility
.\build.ps1 -TestTools             # + DlgTest, MyPEStress, MyPEDump
.\build.ps1 -VsPath D:\BuildTools  # if vswhere cannot find the installation
```
`build.ps1` checks the MSBuild signature, builds `src\WinXShell_VS2015.sln` (Release|x64, static CRT) and compiles
`ui\*.c` with `cl /W4 /WX` (warnings are errors). It writes `out\build-sha256.csv` with the SHA256 and PE machine of
every binary.

Output:
```
out\shell\WinXShell.exe
out\ui\MyPEUI.exe
out\test\DlgTest.exe, MyPEStress.exe, MyPEDump.exe   (-TestTools)
out\shell-build.log, out\ui\ui-build.log
```

## Notes
- The shell project treats the x64 truncation warnings C4311, C4312, C4302, C4244, C4267 and C4090 as **errors**.
  About 24 other warnings remain at the default level (C4996 deprecations, C4018 signed/unsigned); they are benign.
- MSVC binaries are not bit-for-bit reproducible across machines (timestamps, paths). If you keep an allowlist of
  hashes, re-approve after building on a new machine.
- `-ExtraProps <file.props>` imports an extra property sheet after the C++ targets (used for a `/W4` inventory).
