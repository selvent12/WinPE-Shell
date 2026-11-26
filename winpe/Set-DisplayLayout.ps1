<#
.SYNOPSIS
  Runs INSIDE the WinPE image at startup (a MyPEStart run step). Extend mode + native resolution/refresh per monitor + primary/position
  from X:\MyPE\config\display.json. Retries until the GPU driver has enumerated the outputs. Log: X:\MyPE\logs\display.log
  Then the default scale, applied LIVE (MyPEUI.exe scale <percent>; the shell restarts if it already runs):
  "scale" (percent, e.g. 150) or "dpiMode" (uniform100..uniform200) from the first file found:
    <drive>:\MyPE\config\display.json on the boot stick = a fixed/removable drive that has the marker file
    <drive>:\MyPE\mype-stick (editable without a new ISO; the internal disk of the PC being examined is ignored),
    else X:\MyPE\config\display.json (from the image). Only the scale is taken from the file on the drive.
#>
param(
    [string]$Config = 'X:\MyPE\config\display.json',
    [string]$Dll    = 'X:\MyPE\bin\DisplayLayout.dll',
    [string]$Log    = 'X:\MyPE\logs\display.log',
    [int]$TimeoutSec = 60
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null
function Write-Log($m) { $l = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m; Add-Content -Path $Log -Value $l; Write-Host $l }

# default scale (percent) from a display.json: "scale" or "dpiMode"; 0 = none
function Get-ScaleFrom([string]$file) {
    try {
        $j = Get-Content -LiteralPath $file -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($j.PSObject.Properties['scale'] -and [int]$j.scale -ge 100) { return [int]$j.scale }
        if ($j.PSObject.Properties['dpiMode'] -and "$($j.dpiMode)" -match '^uniform(\d+)$') { return [int]$Matches[1] }
    } catch { Write-Log "scale: cannot read $file ($($_.Exception.Message))" }
    0
}
function Set-StartScale {
    $pct = 0; $src = ''
    foreach ($d in [IO.DriveInfo]::GetDrives()) {
        if ($d.Name -like 'X:*' -or $d.DriveType -notin 'Fixed', 'Removable' -or -not $d.IsReady) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $d.RootDirectory.FullName 'MyPE\mype-stick'))) { continue }   # not the boot stick
        $f = Join-Path $d.RootDirectory.FullName 'MyPE\config\display.json'
        if (Test-Path -LiteralPath $f) { $pct = Get-ScaleFrom $f; if ($pct) { $src = $f; break } }
    }
    if (-not $pct) { $pct = Get-ScaleFrom $Config; $src = $Config }
    if (-not $pct) { Write-Log 'scale: no default in the configuration'; return }
    $ui = 'X:\MyPE\MyPEUI.exe'
    if (-not (Test-Path $ui)) { Write-Log "scale: $ui missing, default $pct% not applied"; return }
    $p = Start-Process $ui -ArgumentList 'scale', $pct -PassThru -Wait
    Write-Log "scale: default $pct% from $src -> MyPEUI scale exit=$($p.ExitCode) (details in mypeui.log)"
}

try {
    Add-Type -Path $Dll
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    Write-Log "config: mode=$($cfg.mode) primary=$($cfg.primary) dpiMode=$($cfg.dpiMode)"
    $want = @($cfg.monitors).Count
    $t0 = Get-Date
    do {
        $mons = @([DisplayLayout]::List() | Where-Object { $_.Modes.Count -gt 0 })
        $paths = [DisplayLayout]::AllPathCount()
        Write-Log ("waiting: outputs with modes={0} allPaths={1} adapters={2}" -f $mons.Count, $paths, (($mons | ForEach-Object Adapter | Select-Object -Unique) -join ' / '))
        if ($mons.Count -ge $want -and ($mons.Adapter -notmatch 'Basic Display')) { break }
        Start-Sleep -Seconds 3
    } while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec)

    if ($cfg.mode -eq 'extend') { $rc = [DisplayLayout]::Extend(); Write-Log "SetDisplayConfig(EXTEND) rc=$rc"; Start-Sleep -Seconds 2 }
    $mons = @([DisplayLayout]::List() | Where-Object { $_.Modes.Count -gt 0 })
    foreach ($m in $mons) { Write-Log ("found {0} [{1}] active={2} primary={3} current={4}x{5}@{6} pos={7},{8} modes={9}" -f $m.Device, $m.Adapter, $m.Active, $m.Primary, $m.Width, $m.Height, $m.Hz, $m.X, $m.Y, $m.Modes.Count) }

    if ($mons.Count -lt 2) {
        # Fallback: one monitor (Basic Display / GOP) - use fallbackResolution if offered, never block boot
        $fb = $cfg.fallbackResolution; $m = $mons | Select-Object -First 1
        if ($m) {
            $mode = $m.Modes | Where-Object { $_ -like "$($fb.width)x$($fb.height)@*" } | Sort-Object { [int]($_ -split '@')[1] } -Descending | Select-Object -First 1
            if ($mode) { $hz = [int]($mode -split '@')[1]; $rc = [DisplayLayout]::Stage($m.Device, $fb.width, $fb.height, $hz, 0, 0, $true); $rc2 = [DisplayLayout]::Apply(); Write-Log "fallback $mode on $($m.Device): stage=$rc apply=$rc2" }
            else { Write-Log "fallback: $($fb.width)x$($fb.height) not offered by $($m.Device); keeping current" }
        }
        return
    }
    # map config monitors to outputs by native (largest) mode
    $assign = @{}
    foreach ($c in $cfg.monitors) {
        $cand = $mons | Where-Object { -not $assign.ContainsValue($_.Device) -and ($_.Modes -like "$($c.width)x$($c.height)@*") } |
                Sort-Object { ($_.Modes | ForEach-Object { $w, $r = $_ -split '@'; $a, $b = $w -split 'x'; [int]$a * [int]$b } | Measure-Object -Maximum).Maximum } -Descending | Select-Object -First 1
        if ($cand) { $assign[$c.id] = $cand.Device; Write-Log "map $($c.id) ($($c.width)x$($c.height)) -> $($cand.Device)" } else { Write-Log "WARN no output offers $($c.width)x$($c.height) for $($c.id)" }
    }
    $prim = $cfg.monitors | Where-Object id -eq $cfg.primary
    foreach ($c in $cfg.monitors) {
        if (-not $assign.ContainsKey($c.id)) { continue }
        $dev = $assign[$c.id]; $m = $mons | Where-Object Device -eq $dev
        $hz = $c.refresh; if (-not ($m.Modes -contains "$($c.width)x$($c.height)@$hz")) { $hz = ($m.Modes | Where-Object { $_ -like "$($c.width)x$($c.height)@*" } | ForEach-Object { [int]($_ -split '@')[1] } | Measure-Object -Maximum).Maximum; Write-Log "WARN $($c.id): $($c.refresh) Hz not offered, using $hz Hz" }
        $isPrim = $c.id -eq $cfg.primary
        $x = 0; if (-not $isPrim) { $x = if ($c.position -eq 'left') { -[int]$c.width } else { [int]$prim.width } }
        $rc = [DisplayLayout]::Stage($dev, [int]$c.width, [int]$c.height, [int]$hz, $x, 0, $isPrim)
        Write-Log "stage $($c.id) $dev $($c.width)x$($c.height)@$hz pos=$x,0 primary=$isPrim rc=$rc"
    }
    $rc = [DisplayLayout]::Apply(); Write-Log "apply rc=$rc"
    Start-Sleep -Seconds 2
    foreach ($m in [DisplayLayout]::List() | Where-Object { $_.Active }) { Write-Log ("result {0} {1}x{2}@{3} pos={4},{5} primary={6}" -f $m.Device, $m.Width, $m.Height, $m.Hz, $m.X, $m.Y, $m.Primary) }
} catch { Write-Log "ERROR $($_.Exception.Message)" }
finally { try { Set-StartScale } catch { Write-Log "scale: ERROR $($_.Exception.Message)" } }   # also after the one-monitor fallback (return)
