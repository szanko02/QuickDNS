Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function Convert-ToIco {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SrcPath,
        [Parameter(Mandatory = $true)]
        [string]$DestIcoPath
    )

    if (-not (Test-Path -LiteralPath $SrcPath -PathType Leaf)) {
        throw "Source icon image not found: '$SrcPath'"
    }

    $sizes = @(256, 128, 64, 48, 32, 16)
    $pngList = New-Object System.Collections.ArrayList

    $srcImg = [System.Drawing.Image]::FromFile($SrcPath)
    try {
        foreach ($sz in $sizes) {
            $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try {
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                try {
                    $g.Clear([System.Drawing.Color]::Transparent)
                    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $g.DrawImage($srcImg, 0, 0, $sz, $sz)
                } finally {
                    $g.Dispose()
                }

                $ms = New-Object System.IO.MemoryStream
                try {
                    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
                    $entry = [PSCustomObject]@{
                        Size  = $sz
                        Bytes = $ms.ToArray()
                    }
                    [void]$pngList.Add($entry)
                } finally {
                    $ms.Dispose()
                }
            } finally {
                $bmp.Dispose()
            }
        }
    } finally {
        $srcImg.Dispose()
    }

    $fs = [System.IO.File]::Create($DestIcoPath)
    try {
        $bw = New-Object System.IO.BinaryWriter($fs)
        try {
            # ICONDIR
            $bw.Write([uint16]0) # Reserved
            $bw.Write([uint16]1) # Type (1 = ICO)
            $bw.Write([uint16]$pngList.Count) # Count

            $offset = 6 + ($pngList.Count * 16)

            foreach ($entry in $pngList) {
                $sz = $entry.Size
                $bytes = $entry.Bytes

                $bWidth = if ($sz -ge 256) { [byte]0 } else { [byte]$sz }
                $bHeight = if ($sz -ge 256) { [byte]0 } else { [byte]$sz }

                $bw.Write($bWidth)
                $bw.Write($bHeight)
                $bw.Write([byte]0)   # Color count
                $bw.Write([byte]0)   # Reserved
                $bw.Write([uint16]1) # Color planes
                $bw.Write([uint16]32) # Bit depth
                $bw.Write([uint32]$bytes.Length)
                $bw.Write([uint32]$offset)

                $offset += $bytes.Length
            }

            foreach ($entry in $pngList) {
                $bw.Write($entry.Bytes)
            }

            $bw.Flush()
        } finally {
            $bw.Dispose()
        }
    } finally {
        $fs.Dispose()
    }

    Write-Host "Successfully generated: $DestIcoPath"
}

$rootDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$appIcon = Join-Path $rootDir 'res\app_logo.png'
if (-not (Test-Path -LiteralPath $appIcon -PathType Leaf)) {
    $appIcon = Join-Path $rootDir 'res\app_logo.jpg'
}
$diagIcon = Join-Path $rootDir 'res\diag_logo.jpg'
if (-not (Test-Path -LiteralPath $diagIcon -PathType Leaf)) {
    $diagIcon = $appIcon
}

Convert-ToIco -SrcPath $appIcon -DestIcoPath (Join-Path $rootDir 'res\app_icon.ico')
Convert-ToIco -SrcPath $diagIcon -DestIcoPath (Join-Path $rootDir 'res\diag_icon.ico')
