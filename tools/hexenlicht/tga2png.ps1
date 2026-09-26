# Converts uncompressed 24-bit bottom-up TGA files (the engine's screenshots) to PNG.
param([string[]]$Files, [string]$OutDir)
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($f in $Files) {
    $b = [IO.File]::ReadAllBytes($f)
    $w = $b[12] + 256 * $b[13]; $h = $b[14] + 256 * $b[15]
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $data = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $w, $h), [Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
    for ($y = 0; $y -lt $h; $y++) {
        # TGA row 0 is the bottom row; bitmap rows are top-down, both BGR
        $src = 18 + ($h - 1 - $y) * $w * 3
        [Runtime.InteropServices.Marshal]::Copy($b, $src, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $w * 3)
    }
    $bmp.UnlockBits($data)
    $out = Join-Path $OutDir ([IO.Path]::GetFileNameWithoutExtension($f) + ".png")
    $bmp.Save($out, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "{0} -> {1} ({2}x{3})" -f (Split-Path $f -Leaf), $out, $w, $h
}
