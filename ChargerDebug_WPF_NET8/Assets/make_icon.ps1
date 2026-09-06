Add-Type -AssemblyName System.Drawing

$srcPath = "$PSScriptRoot\pkg_logo_src.png"
$outPath = "$PSScriptRoot\app.ico"

$srcBitmap = [System.Drawing.Bitmap]::new($srcPath)

# Icon sizes needed: 16, 32, 48, 256
$sizes = @(16, 32, 48, 256)

$ms = [System.IO.MemoryStream]::new()
$bw = [System.IO.BinaryWriter]::new($ms)

# Collect PNG data for each size
$pngBytes = @{}
foreach ($sz in $sizes) {
    $resized = [System.Drawing.Bitmap]::new($sz, $sz)
    $g = [System.Drawing.Graphics]::FromImage($resized)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($srcBitmap, 0, 0, $sz, $sz)
    $g.Dispose()

    $pngMs = [System.IO.MemoryStream]::new()
    $resized.Save($pngMs, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBytes[$sz] = $pngMs.ToArray()
    $resized.Dispose()
}

# ICO Header
$bw.Write([uint16]0)       # Reserved
$bw.Write([uint16]1)       # Type: 1 = ICO
$bw.Write([uint16]$sizes.Count) # Count

# Directory entries: 16 bytes each, then image data
# First calculate offsets
$dirOffset = 6 + ($sizes.Count * 16)
$imageOffsets = @()
$runningOffset = $dirOffset
foreach ($sz in $sizes) {
    $imageOffsets += $runningOffset
    $runningOffset += $pngBytes[$sz].Length
}

# Write directory entries
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]
    $data = $pngBytes[$sz]
    # Width (0 = 256)
    $bw.Write([byte]$(if ($sz -eq 256) { 0 } else { $sz }))
    # Height (0 = 256)
    $bw.Write([byte]$(if ($sz -eq 256) { 0 } else { $sz }))
    $bw.Write([byte]0)    # Color count
    $bw.Write([byte]0)    # Reserved
    $bw.Write([uint16]1)  # Planes
    $bw.Write([uint16]32) # Bit count
    $bw.Write([uint32]$data.Length)
    $bw.Write([uint32]$imageOffsets[$i])
}

# Write image data
foreach ($sz in $sizes) {
    $bw.Write($pngBytes[$sz])
}

$bw.Flush()
[System.IO.File]::WriteAllBytes($outPath, $ms.ToArray())
$srcBitmap.Dispose()

Write-Host "Icon created: $outPath ($([System.IO.FileInfo]$outPath).Length bytes)"
