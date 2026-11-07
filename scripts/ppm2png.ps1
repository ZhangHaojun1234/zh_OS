# 把 QEMU 的 screendump PPM 转成 PNG
# 用法: powershell -File scripts\ppm2png.ps1 -Input build\shot.ppm -Output docs\shot.png

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$raw = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Source).Path)

# 解析 P6 头: "P6\n<width> <height>\n<maxval>\n"
$newlines = 0
$offset = 0
for ($i = 0; $i -lt 128 -and $i -lt $raw.Length; $i++) {
    if ($raw[$i] -eq 10) {
        $newlines++
        if ($newlines -eq 3) {
            $offset = $i + 1
            break
        }
    }
}
if ($offset -eq 0) {
    throw "not a P6 PPM file"
}

$header = [System.Text.Encoding]::ASCII.GetString($raw, 0, $offset)
$parts = ($header -split '\s+') | Where-Object { $_ -ne '' }
$width = [int]$parts[1]
$height = [int]$parts[2]

$expected = $offset + $width * $height * 3
if ($raw.Length -lt $expected) {
    throw "truncated PPM: expected $expected bytes, got $($raw.Length)"
}

$bitmap = New-Object System.Drawing.Bitmap($width, $height)
for ($y = 0; $y -lt $height; $y++) {
    for ($x = 0; $x -lt $width; $x++) {
        $idx = $offset + ($y * $width + $x) * 3
        $color = [System.Drawing.Color]::FromArgb($raw[$idx], $raw[$idx + 1], $raw[$idx + 2])
        $bitmap.SetPixel($x, $y, $color)
    }
}

$outFull = [System.IO.Path]::GetFullPath($Destination)
$outDir = [System.IO.Path]::GetDirectoryName($outFull)
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$bitmap.Save($outFull, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

Write-Host ("  {0} -> {1} ({2}x{3})" -f $Source, $Destination, $width, $height)
