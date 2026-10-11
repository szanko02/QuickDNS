Add-Type -AssemblyName System.Drawing

function Convert-ToIco {
    param(
        [string]$srcPath,
        [string]$destIcoPath
    )

    $srcImg = [System.Drawing.Image]::FromFile($srcPath)
    $sizes = @(256, 128, 64, 48, 32, 16)
    $pngList = New-Object System.Collections.ArrayList

    foreach ($sz in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap($sz, $sz)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($srcImg, 0, 0, $sz, $sz)
        $g.Dispose()

        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        
        $entry = [PSCustomObject]@{
            Size = $sz
            Bytes = $ms.ToArray()
        }
        [void]$pngList.Add($entry)
        $ms.Dispose()
    }
    $srcImg.Dispose()

    $fs = [System.IO.File]::Create($destIcoPath)
    $bw = New-Object System.IO.BinaryWriter($fs)

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
        $bw.Write([byte]0) # Color count
        $bw.Write([byte]0) # Reserved
        $bw.Write([uint16]1) # Color planes
        $bw.Write([uint16]32) # Bit depth
        $bw.Write([uint32]$bytes.Length) # Image size
        $bw.Write([uint32]$offset) # Image offset

        $offset += $bytes.Length
    }

    foreach ($entry in $pngList) {
        $bytes = $entry.Bytes
        $bw.Write($bytes)
    }

    $bw.Flush()
    $bw.Close()
    $fs.Close()
    Write-Host "Successfully generated: $destIcoPath"
}

$appIcon = "C:\Users\SWEETY\.gemini\antigravity-cli\brain\b586bf1c-c888-4572-903c-9f6fd6bafed3\quickdns_app_icon_1791729717789.jpg"
$diagIcon = "C:\Users\SWEETY\.gemini\antigravity-cli\brain\b586bf1c-c888-4572-903c-9f6fd6bafed3\dns_diagnostic_icon_1791729716035.jpg"

Convert-ToIco -srcPath $appIcon -destIcoPath "C:\Users\SWEETY\QuickDNS\res\app_icon.ico"
Convert-ToIco -srcPath $diagIcon -destIcoPath "C:\Users\SWEETY\QuickDNS\res\diag_icon.ico"
