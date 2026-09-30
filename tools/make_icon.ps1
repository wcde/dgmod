# Generates res/app.ico (PNG-compressed multi-size icon) using System.Drawing.
param([string]$Out = "$PSScriptRoot/../res/app.ico")
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$frames = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $r = [single]($s * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $m = [single]($s * 0.04); $w = [single]($s - 2 * $m)
    $path.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($m + $w - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($m + $w - 2 * $r, $m + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($m, $m + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 0, 140, 170)), ([System.Drawing.Color]::FromArgb(255, 40, 90, 210)), 45.0
    $g.FillPath($brush, $path)
    # smooth sine (the resampled signal) with coarse sample points (the source)
    $pw = [single][Math]::Max(1.0, $s / 14.0)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), $pw
    $pen.LineJoin = 'Round'; $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $curve = @()
    for ($i = 0; $i -le 32; $i++) {
        $x = 0.16 + 0.68 * $i / 32
        $y = 0.52 - 0.24 * [Math]::Sin(2 * [Math]::PI * $i / 32)
        $curve += [System.Drawing.PointF]::new($s * $x, $s * $y)
    }
    $g.DrawLines($pen, [System.Drawing.PointF[]]$curve)
    if ($s -ge 24) {
        $dot = [single][Math]::Max(2.0, $s / 9.0)
        $dotBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 214, 102))
        foreach ($k in 0, 8, 16, 24, 32) {
            $pt = $curve[$k]
            $g.FillEllipse($dotBrush, $pt.X - $dot / 2, $pt.Y - $dot / 2, $dot, $dot)
        }
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , @($s, $ms.ToArray())
    $bmp.Dispose()
}

$fs = [System.IO.File]::Create((Resolve-Path -LiteralPath (Split-Path $Out)).Path + "/" + (Split-Path $Out -Leaf))
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $s = $f[0]; $data = $f[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$data.Length); $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($f in $frames) { $bw.Write([byte[]]$f[1]) }
$bw.Close()
Write-Output "Wrote $Out"
