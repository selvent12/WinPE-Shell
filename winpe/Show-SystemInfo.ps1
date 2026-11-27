<#
.SYNOPSIS
  Runs INSIDE the WinPE image: system information as text, shown in Notepad.
  Started by WinPE-ShellUI.exe open sysinfo (winpe-shellui.ini [features] sysinfo=<this script>): Start menu "System information",
  desktop menu "System properties". Report: <base>\logs\sysinfo.txt (<base> = the folder above this script's bin\;
  also kept for later reading).
  Only reads (CIM/WMI, registry, Storage cmdlets); every section is independent: a failing source is reported, the rest
  still comes.
#>
param([string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\sysinfo.txt'), [switch]$NoShow)
$ErrorActionPreference = 'Stop'
$lines = New-Object System.Collections.Generic.List[string]
function Add([string]$s = '') { $lines.Add($s) }
function Head([string]$t) { Add ''; Add $t; Add ('-' * $t.Length) }
function GB($b) { if ($null -ne $b) { '{0:N1} GB' -f ($b / 1GB) } else { '?' } }   # 0 bytes free is "0.0 GB", not unknown
function Section([string]$title, [scriptblock]$body) {
    Head $title
    try { & $body } catch { Add "  (not available: $($_.Exception.Message))" }
}

Add "System information ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))"

Section 'System' {
    $cs = Get-CimInstance Win32_ComputerSystem
    $os = Get-CimInstance Win32_OperatingSystem
    Add "  Computer:      $($cs.Manufacturer) $($cs.Model)"
    Add "  Name:          $($env:COMPUTERNAME)  (workgroup: $($cs.Workgroup))"
    Add "  Windows PE:    $($os.Caption) $($os.Version) build $($os.BuildNumber), $($os.OSArchitecture)"
    $up = (Get-Date) - $os.LastBootUpTime
    Add ("  Started:       {0:yyyy-MM-dd HH:mm}  (up {1}h {2:D2}m)" -f $os.LastBootUpTime, [int][Math]::Floor($up.TotalHours), $up.Minutes)
}

Section 'Time' {   # PC test 2026-11-27: PE 2 h behind the BIOS clock on one PC; what does Windows think?
    $tz = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\TimeZoneInformation' -ErrorAction SilentlyContinue
    $now = Get-Date
    Add ("  Local time:    {0:yyyy-MM-dd HH:mm:ss}" -f $now)
    Add ("  UTC:           {0:yyyy-MM-dd HH:mm:ss}" -f $now.ToUniversalTime())
    Add "  Time zone:     $([TimeZoneInfo]::Local.Id) ($([TimeZoneInfo]::Local.BaseUtcOffset)), registry key '$($tz.TimeZoneKeyName)', Bias $($tz.Bias), ActiveTimeBias $($tz.ActiveTimeBias)"
    Add "  BIOS clock:    read as $(if ($tz.RealTimeIsUniversal -eq 1) { 'UTC (RealTimeIsUniversal=1)' } else { 'local time (RealTimeIsUniversal=0)' }) - compare with the time shown in the BIOS setup"
}

Section 'Firmware' {
    $bios = Get-CimInstance Win32_BIOS
    $fw = $env:firmware_type
    if (-not $fw) { $fw = if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State') { 'UEFI' } else { 'BIOS (legacy)' } }
    Add "  Type:          $fw"
    $sb = 'unknown'
    try { $sb = if (Confirm-SecureBootUEFI) { 'ON' } else { 'OFF' } } catch {
        try { $v = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction Stop).UEFISecureBootEnabled; $sb = if ($v -eq 1) { 'ON' } else { 'OFF' } } catch { }
    }
    Add "  Secure Boot:   $sb"
    Add "  BIOS/UEFI:     $($bios.Manufacturer) $($bios.SMBIOSBIOSVersion)$(if ($bios.ReleaseDate) { ', ' + $bios.ReleaseDate.ToString('yyyy-MM-dd') })"
    try { $bb = Get-CimInstance Win32_BaseBoard; Add "  Motherboard:   $($bb.Manufacturer) $($bb.Product)" } catch { }
}

Section 'Processor' {
    foreach ($c in Get-CimInstance Win32_Processor) {
        Add "  $($c.Name.Trim())"
        Add "    $($c.NumberOfCores) cores, $($c.NumberOfLogicalProcessors) threads, max $($c.MaxClockSpeed) MHz"
    }
}

Section 'Memory' {
    $cs = Get-CimInstance Win32_ComputerSystem
    $os = Get-CimInstance Win32_OperatingSystem
    Add "  Installed:     $(GB $cs.TotalPhysicalMemory)  (free now: $(GB ($os.FreePhysicalMemory * 1KB)))"
    $arr = @(Get-CimInstance Win32_PhysicalMemoryArray -ErrorAction SilentlyContinue)
    $slots = ($arr | Measure-Object MemoryDevices -Sum).Sum
    $mods = @(Get-CimInstance Win32_PhysicalMemory)
    Add "  Slots:         $($mods.Count) used$(if ($slots) { " of $slots" })"
    foreach ($m in $mods) {
        $speed = if ($m.ConfiguredClockSpeed) { $m.ConfiguredClockSpeed } else { $m.Speed }
        Add ("    {0,-14} {1,8}  {2} MT/s  {3} {4}" -f $m.DeviceLocator, (GB $m.Capacity), $speed, "$($m.Manufacturer)".Trim(), "$($m.PartNumber)".Trim())
    }
}

Section 'Graphics' {
    $vc = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)
    foreach ($v in $vc) {
        Add "  $($v.Name)"
        $res = if ($v.CurrentHorizontalResolution) { "$($v.CurrentHorizontalResolution)x$($v.CurrentVerticalResolution) @ $($v.CurrentRefreshRate) Hz" } else { 'not active' }
        Add "    driver $($v.DriverVersion), $res"
    }
    if (-not $vc) {     # WinPE: often no WMI video provider -> PnP display class + the screens Windows reports
        foreach ($p in Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Display'" -ErrorAction SilentlyContinue) { Add "  $($p.Name)  [$($p.Status)]" }
        Add-Type -AssemblyName System.Windows.Forms
        foreach ($s in [System.Windows.Forms.Screen]::AllScreens) { Add ("    screen {0}: {1}x{2}{3}" -f $s.DeviceName, $s.Bounds.Width, $s.Bounds.Height, $(if ($s.Primary) { ' (main)' })) }
    }
}

Section 'Disks' {
    $pd = @{}
    try { foreach ($p in Get-PhysicalDisk) { $pd["$($p.DeviceId)"] = $p } } catch { }
    foreach ($d in Get-CimInstance Win32_DiskDrive | Sort-Object Index) {
        $p = $pd["$($d.Index)"]
        $kind = if ($p) { "$($p.BusType) $($p.MediaType)".Replace('Unspecified', '').Trim() } else { $d.InterfaceType }
        Add ("  Disk {0}: {1}  {2}  ({3})" -f $d.Index, "$($d.Model)".Trim(), (GB $d.Size), $kind)
    }
    Add ''
    # Win32_LogicalDisk: works in WinPE without the Storage module (Get-Volume returned nothing there)
    foreach ($v in Get-CimInstance Win32_LogicalDisk | Where-Object { $_.DriveType -in 2, 3 -and $_.Size } | Sort-Object DeviceID) {
        $kind = if ($v.DriveType -eq 2) { 'removable' } else { 'fixed' }
        Add ("  {0}  {1,-16} {2,-6} {3,10} free of {4}  ({5})" -f $v.DeviceID, $v.VolumeName, $v.FileSystem, (GB $v.FreeSpace), (GB $v.Size), $kind)
    }
}

Section 'Network adapters' {
    $n = @(Get-CimInstance Win32_NetworkAdapter | Where-Object { $_.PhysicalAdapter })
    if (-not $n) { Add '  none (or no driver)' }
    foreach ($a in $n) { Add "  $($a.Name)  [$(if ($a.NetEnabled) { 'enabled' } else { 'disabled' })]" }
}

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[IO.File]::WriteAllLines($Out, $lines, (New-Object Text.UTF8Encoding $true))
if (-not $NoShow) { Start-Process X:\Windows\System32\notepad.exe -ArgumentList "`"$Out`"" }
