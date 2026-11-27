<#
.SYNOPSIS  Generates a desktop background for WinPE-Shell (own drawing, no third-party image).
           -Style hills (default): a bright landscape - blue sky gradient with soft clouds over rolling green hills.
           -Style dark: the former dark diagonal gradient with a soft blue glow.
           Output BMP or JPG (the shell loads those). The shell scales the picture to the screen and picks the first
           existing file of JS_DESKTOP.wallpaper (a list, "*:\" = the boot stick), so a picture on the stick replaces
           this default without a new image.
#>
param([Parameter(Mandatory)][string]$Out, [int]$Width = 3840, [int]$Height = 2160, [ValidateSet('hills', 'dark')][string]$Style = 'hills')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function P([double]$x, [double]$y) { New-Object System.Drawing.PointF ([float]($x * $Width)), ([float]($y * $Height)) }
$bmp = New-Object System.Drawing.Bitmap $Width, $Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
try {
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
    $rect = New-Object System.Drawing.Rectangle 0, 0, $Width, $Height
    if ($Style -eq 'dark') {
        $lin = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, (C 255 36 31 49), (C 255 18 38 58), 35.0
        $g.FillRectangle($lin, $rect); $lin.Dispose()
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $path.AddEllipse([int]($Width * 0.45), [int]($Height * 0.35), [int]($Width * 0.9), [int]($Height * 1.1))
        $pgb = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
        $pgb.CenterColor = C 90 53 132 228; $pgb.SurroundColors = @(C 0 53 132 228)
        $g.FillPath($pgb, $path); $pgb.Dispose(); $path.Dispose()
    } else {
        # sky: deep blue at the top, pale near the horizon
        $sky = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, (C 255 30 92 186), (C 255 196 226 248), 90.0
        $blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
        $blend.Colors = @((C 255 30 92 186), (C 255 92 158 226), (C 255 196 226 248)); $blend.Positions = @([float]0, [float]0.45, [float]1)
        $sky.InterpolationColors = $blend
        $g.FillRectangle($sky, $rect); $sky.Dispose()
        # soft clouds: clusters of translucent ellipses (fixed seed: the same picture at every build)
        $rnd = New-Object System.Random 20261127
        foreach ($c in @(@(0.16, 0.18, 1.0), @(0.47, 0.11, 0.8), @(0.74, 0.22, 1.15), @(0.92, 0.08, 0.6), @(0.33, 0.30, 0.55))) {
            $cx = $c[0] * $Width; $cy = $c[1] * $Height; $s = $c[2] * $Height * 0.075
            for ($i = 0; $i -lt 22; $i++) {
                $w = $s * (1.8 + $rnd.NextDouble() * 2.0); $h = $s * (0.9 + $rnd.NextDouble() * 0.7)
                $x = $cx + ($rnd.NextDouble() - 0.5) * $s * 5; $y = $cy + ($rnd.NextDouble() - 0.5) * $s * 1.1
                # each puff: white in the middle, fading to transparent at its edge (no hard outline)
                $ep = New-Object System.Drawing.Drawing2D.GraphicsPath
                $ep.AddEllipse([float]($x - $w / 2), [float]($y - $h / 2), [float]$w, [float]$h)
                $br = New-Object System.Drawing.Drawing2D.PathGradientBrush $ep
                $br.CenterColor = C (70 + $rnd.Next(50)) 255 255 255; $br.SurroundColors = @(C 0 255 255 255)
                $g.FillPath($br, $ep); $br.Dispose(); $ep.Dispose()
            }
        }
        # rear hill: muted green, gentle curve
        $back = New-Object System.Drawing.Drawing2D.GraphicsPath
        $back.AddBeziers([System.Drawing.PointF[]]@((P 0 0.60), (P 0.22 0.50), (P 0.42 0.56), (P 0.60 0.60), (P 0.76 0.64), (P 0.90 0.52), (P 1 0.55)))
        $back.AddLine((P 1 0.55), (P 1 1)); $back.AddLine((P 1 1), (P 0 1)); $back.CloseFigure()
        $bb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, ([int]($Height * 0.48)), $Width, ([int]($Height * 0.52) + 2)), (C 255 92 150 74), (C 255 46 104 40), 90.0
        $g.FillPath($bb, $back); $bb.Dispose(); $back.Dispose()
        # front hill: bright green, a large rounded crest left of the centre
        $front = New-Object System.Drawing.Drawing2D.GraphicsPath
        $front.AddBeziers([System.Drawing.PointF[]]@((P 0 0.80), (P 0.18 0.62), (P 0.40 0.60), (P 0.58 0.70), (P 0.74 0.79), (P 0.88 0.80), (P 1 0.74)))
        $front.AddLine((P 1 0.74), (P 1 1)); $front.AddLine((P 1 1), (P 0 1)); $front.CloseFigure()
        $fb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Rectangle 0, ([int]($Height * 0.58)), $Width, ([int]($Height * 0.42) + 2)), (C 255 126 196 64), (C 255 54 128 34), 90.0
        $g.FillPath($fb, $front); $fb.Dispose()
        # sunlight on the crest
        $glow = New-Object System.Drawing.Drawing2D.GraphicsPath
        $glow.AddEllipse([float]($Width * 0.12), [float]($Height * 0.56), [float]($Width * 0.42), [float]($Height * 0.22))
        $gb = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
        $gb.CenterColor = C 70 236 255 170; $gb.SurroundColors = @(C 0 236 255 170)
        $g.SetClip($front); $g.FillPath($gb, $glow); $g.ResetClip(); $gb.Dispose(); $glow.Dispose(); $front.Dispose()
    }
} finally { $g.Dispose() }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$fmt = switch ([IO.Path]::GetExtension($Out).ToLower()) { '.bmp' { 'Bmp' } { $_ -in '.jpg', '.jpeg' } { 'Jpeg' } default { 'Png' } }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::$fmt); $bmp.Dispose()   # the shell (OleLoadPicture): BMP/JPG only
"wallpaper ($Style): $Out"
