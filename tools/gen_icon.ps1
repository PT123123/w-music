# Generates the app icon: a green single note (U+266A) on a black circle.
# Produces a multi-size .ico (PNG-compressed entries) at src\w-music\Assets\app.ico.
#
# Run:  pwsh -NoProfile tools\gen_icon.ps1

Add-Type -AssemblyName System.Drawing

$here = $PSScriptRoot
$outDir = Join-Path (Resolve-Path (Join-Path $here '..\src\w-music')) 'Assets'
$icoPath = Join-Path $outDir 'app.ico'
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$noteGreen = [System.Drawing.Color]::FromArgb(0x31, 0xC2, 0x7C)

function New-NoteBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # black circle (tiny margin so the round shape survives downsampling)
    $margin = [int][math]::Max(1, [math]::Floor($size * 0.04))
    $circleW = $size - 2 * $margin
    $circleH = $size - 2 * $margin
    $circleRect = New-Object System.Drawing.RectangleF ($margin, $margin, $circleW, $circleH)
    $blackBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)
    $g.FillEllipse($blackBrush, $circleRect)

    # green note, centred (nudge down a touch: the glyph's ink sits high)
    $fontSize = $size * 0.58
    $font = New-Object System.Drawing.Font('Segoe UI Symbol', $fontSize, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $greenBrush = New-Object System.Drawing.SolidBrush($noteGreen)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textX = [single]0
    $textY = [single]($size * 0.06)
    $textW = [single]$size
    $textH = [single]($size * 0.94)
    $textRect = New-Object System.Drawing.RectangleF ($textX, $textY, $textW, $textH)
    $g.DrawString([char]0x266A, $font, $greenBrush, $textRect, $fmt)

    $fmt.Dispose()
    $greenBrush.Dispose()
    $font.Dispose()
    $blackBrush.Dispose()
    $g.Dispose()
    return $bmp
}

function Save-PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

# ---- render every size to PNG bytes ----
$pngs = @{}
foreach ($s in $sizes) {
    $bmp = New-NoteBitmap $s
    $pngs[$s] = Save-PngBytes $bmp
    $bmp.Dispose()
    Write-Host "rendered ${s}x${s} (PNG $($pngs[$s].Length) bytes)"
}

# ---- assemble the .ico container (PNG entries, Vista+) ----
$count = $sizes.Count
$ico = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($ico)

$headerSize = 6 + 16 * $count
$offset = $headerSize

# ICONDIR
$writer.Write([UInt16]0)       # reserved
$writer.Write([UInt16]1)       # type: icon
$writer.Write([UInt16]$count)

# ICONDIRENTRY * count
foreach ($s in $sizes) {
    if ($s -ge 256) { $dim = 0 } else { $dim = $s }   # 0 means 256
    $writer.Write([byte]$dim)    # width
    $writer.Write([byte]$dim)    # height
    $writer.Write([byte]0)       # palette count
    $writer.Write([byte]0)       # reserved
    $writer.Write([UInt16]1)     # planes
    $writer.Write([UInt16]32)    # bit count
    $writer.Write([UInt32]$pngs[$s].Length)
    $writer.Write([UInt32]$offset)
    $offset += $pngs[$s].Length
}

# image data
foreach ($s in $sizes) {
    $writer.Write([byte[]]$pngs[$s])
}

$writer.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $ico.ToArray())
$writer.Dispose()
$ico.Dispose()
Write-Host "wrote $icoPath ($($(Get-Item $icoPath).Length) bytes)"
