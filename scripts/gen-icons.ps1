# Generates the LeanCast icon (build/icon.ico, build/icon.png, build/tray.png)
# from the checked-in source artwork.
Add-Type -AssemblyName System.Drawing

$buildDir = Join-Path $PSScriptRoot "..\build"
$assetPath = Join-Path $PSScriptRoot "..\native\assets\icon.png"
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

if (-not (Test-Path $assetPath)) {
  throw "Icon source not found: $assetPath"
}

function New-IconBitmap([System.Drawing.Image]$source, [int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($source, 0, 0, $size, $size)
  $g.Dispose()
  return $bmp
}

$source = [System.Drawing.Image]::FromFile((Resolve-Path $assetPath))

# 256px PNG (for ICO and window icon)
$big = New-IconBitmap $source 256
$pngPath = Join-Path $buildDir "icon.png"
$big.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)

# PNG bytes for the ICO container
$ms = New-Object System.IO.MemoryStream
$big.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$pngBytes = $ms.ToArray()
$ms.Dispose()

# ICO with embedded PNG (Vista+ supports PNG-in-ICO)
$icoPath = Join-Path $buildDir "icon.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)      # reserved
$bw.Write([uint16]1)      # type = icon
$bw.Write([uint16]1)      # image count
$bw.Write([byte]0)        # width 0 => 256
$bw.Write([byte]0)        # height 0 => 256
$bw.Write([byte]0)        # colors
$bw.Write([byte]0)        # reserved
$bw.Write([uint16]1)      # planes
$bw.Write([uint16]32)     # bits per pixel
$bw.Write([uint32]$pngBytes.Length)
$bw.Write([uint32]22)     # offset (6 + 16)
$bw.Write($pngBytes)
$bw.Flush(); $bw.Dispose(); $fs.Dispose()

# 32px tray PNG
$tray = New-IconBitmap $source 32
$trayPath = Join-Path $buildDir "tray.png"
$tray.Save($trayPath, [System.Drawing.Imaging.ImageFormat]::Png)

$big.Dispose(); $tray.Dispose(); $source.Dispose()
Write-Host "Icons generated in $buildDir : icon.ico, icon.png, tray.png"
