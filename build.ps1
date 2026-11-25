<#
.SYNOPSIS  Builds WinPE-Shell from source: the shell (src\, MSBuild, v143, x64 Release), the UI utility (ui\, cl) and the
           optional test tools (tools\test\). Microsoft Build Tools only; no NuGet, vcpkg or network access.
.DESCRIPTION
           Output (default .\out):
             out\shell\WinXShell.exe      the shell (desktop, taskbar, Start menu, tray)
             out\ui\MyPEUI.exe            shutdown dialog, Display window, network ON/OFF + Wi-Fi, ms-settings handler
             out\test\*.exe               DlgTest, MyPEStress, MyPEDump (only with -TestTools; for VM tests)
             out\build-sha256.csv         SHA256 + PE machine of every binary
           Requirements: Visual Studio 2022 Build Tools with "Desktop development with C++" (MSVC v143) and the
           Windows 11 SDK 10.0.26100. See docs\BUILD.md.
.EXAMPLE   .\build.ps1
.EXAMPLE   .\build.ps1 -VsPath 'D:\BuildTools' -TestTools
#>
param(
    [string]$VsPath,                 # Build Tools / Visual Studio root; default: found with vswhere
    [string]$Out = (Join-Path $PSScriptRoot 'out'),
    [switch]$TestTools,
    [string]$ExtraProps = ''         # optional .props imported after the C++ targets (e.g. a /W4 inventory)
)
$ErrorActionPreference = 'Stop'
$env:VSCMD_SKIP_SENDTELEMETRY = '1'; $env:MSBUILDDISABLENODEREUSE = '1'; $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'

# ---- locate MSVC ----
if (-not $VsPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install VS 2022 Build Tools or pass -VsPath' }
    $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsPath) { throw 'no Visual Studio / Build Tools with the C++ x64 tools found (pass -VsPath)' }
}
$msbuild = Join-Path $VsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
foreach ($f in $msbuild, $vcvars) { if (-not (Test-Path $f)) { throw "missing: $f" } }
if ((Get-AuthenticodeSignature $msbuild).Status -ne 'Valid') { throw "MSBuild signature not valid: $msbuild" }

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force "$Out\shell", "$Out\ui", $obj | Out-Null

# ---- 1) shell (MSBuild) ----
if ($ExtraProps -and -not (Test-Path $ExtraProps)) { throw "ExtraProps not found: $ExtraProps" }
$log = Join-Path $Out 'shell-build.log'
& $msbuild (Join-Path $PSScriptRoot 'src\WinXShell_VS2015.sln') /m /nologo /v:minimal /nodeReuse:false `
    /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$Out\shell\" "/p:IntDir=$obj\shell\" `
    /p:ForceImportBeforeCppTargets= "/p:ForceImportAfterCppTargets=$ExtraProps" "/flp:LogFile=$log;Verbosity=normal" | Select-Object -Last 10
if ($LASTEXITCODE) { throw "MSBuild failed ($LASTEXITCODE), see $log" }
'shell warnings: ' + (Select-String $log -Pattern ': warning ' | Measure-Object).Count

# ---- 2) UI utility + test tools (cl) ----
& cmd.exe /d /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] } }
$uiSrc = 'MyPEUI.c', 'display.c', 'dispcfg.c', 'brightness.c', 'net.c', 'scale.c' | ForEach-Object { Join-Path $PSScriptRoot "ui\$_" }
$uiLibs = 'kernel32.lib', 'user32.lib', 'gdi32.lib', 'shell32.lib', 'advapi32.lib', 'comctl32.lib', 'ole32.lib', 'oleaut32.lib', 'wbemuuid.lib', 'iphlpapi.lib', 'setupapi.lib', 'cfgmgr32.lib', 'newdev.lib', 'dnsapi.lib'
Push-Location "$Out\ui"
try {
    Get-ChildItem -Filter *.obj | Remove-Item -Force
    # /WX: warnings are errors (including the x64 truncation family C4311/C4312/C4302/C4244/C4267/C4090 from /W4)
    & cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /DUNICODE /D_UNICODE $uiSrc /Fe:MyPEUI.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /guard:cf $uiLibs 2>&1 | Out-File ui-build.log -Encoding UTF8
    if ($LASTEXITCODE) { Get-Content ui-build.log | Select-Object -Last 20; throw "cl failed for MyPEUI ($LASTEXITCODE)" }
    Get-ChildItem -Filter *.obj | Remove-Item -Force
} finally { Pop-Location }
if ($TestTools) {
    New-Item -ItemType Directory -Force "$Out\test" | Out-Null
    Push-Location "$Out\test"
    try {
        foreach ($t in @(@('MyPEStress', '/SUBSYSTEM:CONSOLE user32.lib'), @('MyPEDump', '/SUBSYSTEM:CONSOLE dbghelp.lib'), @('DlgTest', '/SUBSYSTEM:CONSOLE'), @('DpiTest', '/SUBSYSTEM:CONSOLE user32.lib shcore.lib advapi32.lib'))) {
            $a = @('/nologo', '/W4', '/O2', '/MT', '/GS', '/DUNICODE', '/D_UNICODE', (Join-Path $PSScriptRoot "tools\test\$($t[0]).c"), "/Fe:$($t[0]).exe", '/link') + ($t[1] -split ' ')
            & cl.exe @a 2>&1 | Where-Object { $_ -match 'warning|error' }
            if ($LASTEXITCODE) { throw "cl failed for $($t[0])" }
        }
        Get-ChildItem -Filter *.obj | Remove-Item -Force
    } finally { Pop-Location }
}

# ---- 3) hashes + architecture ----
$rows = Get-ChildItem $Out -Recurse -File -Include *.exe, *.dll | Where-Object FullName -notlike "$obj\*" | ForEach-Object {
    $b = [IO.File]::ReadAllBytes($_.FullName); $pe = [BitConverter]::ToInt32($b, 0x3C)
    [pscustomobject]@{ File = $_.FullName.Substring($Out.Length + 1); Size = $_.Length; SHA256 = (Get-FileHash $_.FullName).Hash; Machine = '0x{0:X4}' -f [BitConverter]::ToUInt16($b, $pe + 4) }
}
$rows | Format-Table -AutoSize | Out-String -Width 200
$rows | Export-Csv (Join-Path $Out 'build-sha256.csv') -NoTypeInformation -Encoding UTF8
