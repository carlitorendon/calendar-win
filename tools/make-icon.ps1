# Generates assets\calendar.ico (16–256 px, PNG-compressed entries) for the app.
# Each size is drawn separately so small sizes stay crisp. Run from any directory:
#   pwsh -File tools\make-icon.ps1
param([string]$Out = (Join-Path $PSScriptRoot '..\assets\calendar.ico'))

Add-Type -AssemblyName System.Drawing

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $d = [Math]::Min($r * 2, [Math]::Min($w, $h))
    if ($d -le 0) { $p.AddRectangle([System.Drawing.RectangleF]::new($x, $y, $w, $h)); return $p }
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function C([string]$hex, [int]$a = 255) {
    $v = [Convert]::ToInt32($hex.TrimStart('#'), 16)
    [System.Drawing.Color]::FromArgb($a, ($v -shr 16) -band 255, ($v -shr 8) -band 255, $v -band 255)
}

function Draw-Icon([int]$size) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    $s = $size / 256.0
    $small = $size -le 24
    $rings = $size -ge 32

    # Page geometry (designed on a 256 grid)
    $px = 22 * $s; $pw = 212 * $s
    $py = ($rings ? 38 : 26) * $s
    $ph = 232 * $s - $py
    if ($small) { $px = 1; $py = 1.5; $pw = $size - 2; $ph = $size - 2.5 }
    $r = [Math]::Max(1.5, 40 * $s)
    $headerH = $small ? [Math]::Round($ph * 0.34) : 62 * $s

    $page = New-RoundRect $px $py $pw $ph $r

    # Soft shadow (larger sizes only)
    if ($size -ge 48) {
        foreach ($i in 1..4) {
            $sh = New-RoundRect ($px) ($py + $i * 2 * $s) $pw $ph $r
            $b = [System.Drawing.SolidBrush]::new((C '#000000' 14))
            $g.FillPath($b, $sh); $b.Dispose(); $sh.Dispose()
        }
    }

    # White page body
    $bodyBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.PointF]::new(0, $py), [System.Drawing.PointF]::new(0, $py + $ph + 1), (C '#FFFFFF'), (C '#E9EDF2'))
    $g.FillPath($bodyBrush, $page)

    # Blue header band, clipped to the page shape
    $g.SetClip($page)
    $hdrBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.PointF]::new(0, $py), [System.Drawing.PointF]::new(0, $py + $headerH + 1), (C '#4CC2FF'), (C '#0F6CBD'))
    $g.FillRectangle($hdrBrush, $px, $py, $pw, $headerH)
    $g.ResetClip()

    # Outline for contrast on light and dark taskbars
    $pen = [System.Drawing.Pen]::new((C '#0B3D6E' ($small ? 70 : 55)), [Math]::Max(1, 2.5 * $s))
    $g.DrawPath($pen, $page)

    # Binder rings
    if ($rings) {
        foreach ($cx in 82, 174) {
            $rw = 18 * $s; $rh = 50 * $s
            $ring = New-RoundRect ($cx * $s - $rw / 2) (18 * $s) $rw $rh ($rw / 2)
            $rb = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
                [System.Drawing.PointF]::new($cx * $s - $rw / 2, 0), [System.Drawing.PointF]::new($cx * $s + $rw / 2 + 1, 0), (C '#F4F6F8'), (C '#9AA6B2'))
            $g.FillPath($rb, $ring)
            $rp = [System.Drawing.Pen]::new((C '#0B3D6E' 90), [Math]::Max(1, 2 * $s))
            $g.DrawPath($rp, $ring)
            $rb.Dispose(); $rp.Dispose(); $ring.Dispose()
        }
    }

    # Date grid with one highlighted day
    $cols = $small ? 3 : 4
    $rows = $small ? 2 : 3
    $gx0 = $px + $pw * 0.14; $gx1 = $px + $pw * 0.86
    $gy0 = $py + $headerH + ($ph - $headerH) * ($small ? 0.2 : 0.17)
    $gy1 = $py + $ph - ($ph - $headerH) * ($small ? 0.2 : 0.15)
    $cw = ($gx1 - $gx0) / $cols; $ch = ($gy1 - $gy0) / $rows
    $dot = [Math]::Min($cw, $ch) * ($small ? 0.62 : 0.56)
    $cellBrush = [System.Drawing.SolidBrush]::new((C '#B8C2CC'))
    $hiBrush = [System.Drawing.SolidBrush]::new((C '#0F6CBD'))
    $hiRow = $small ? 1 : 1; $hiCol = $small ? 1 : 2
    for ($rr = 0; $rr -lt $rows; $rr++) {
        for ($cc = 0; $cc -lt $cols; $cc++) {
            $cx = $gx0 + ($cc + 0.5) * $cw; $cy = $gy0 + ($rr + 0.5) * $ch
            if ($rr -eq $hiRow -and $cc -eq $hiCol) {
                $hd = $dot * ($small ? 1.15 : 1.5)
                $g.FillEllipse($hiBrush, $cx - $hd / 2, $cy - $hd / 2, $hd, $hd)
            } else {
                $cell = New-RoundRect ($cx - $dot / 2) ($cy - $dot / 2) $dot $dot ([Math]::Max(0.5, $dot * 0.22))
                $g.FillPath($cellBrush, $cell); $cell.Dispose()
            }
        }
    }

    $g.Dispose()
    $ms = [System.IO.MemoryStream]::new()
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($sz in $sizes) { , (Draw-Icon $sz) }

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$fs = [System.IO.File]::Create($Out)
$w = [System.IO.BinaryWriter]::new($fs)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]; $data = $images[$i]
    $w.Write([byte]($sz -ge 256 ? 0 : $sz)); $w.Write([byte]($sz -ge 256 ? 0 : $sz))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$data.Length); $w.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($data in $images) { $w.Write($data) }
$w.Dispose()
Write-Host "Wrote $Out"
