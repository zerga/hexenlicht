# Compares two folders of the engine's TGA screenshots file by file (same names).
# Prints per file: pixels that differ at all, pixels differing by more than -Threshold
# in some channel, the largest channel difference and the bounding box of the
# differences (x right, y down). With -DiffDir, writes a PNG per differing file:
# the A image darkened, differing pixels red. -MaxY: compare only the rows above it (e.g. to skip the HUD).
# -Noise: a second run of A (same file names); pixels that differ between the two runs are not compared.
param([string]$A, [string]$B, [int]$Threshold = 8, [string]$DiffDir = '', [int]$MaxY = 100000, [string]$Noise = '')
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
public static class TgaDiff {
    // returns { differing, over threshold, max diff, x0, y0, x1, y1 } (y from the top);
    // img (w * h * 3, top-down BGR) gets the diff picture if not null
    public static int[] Compare(byte[] a, byte[] b, int threshold, byte[] img, int maxY, byte[] n) {
        int w = a[12] + 256 * a[13], h = a[14] + 256 * a[15];
        int any = 0, over = 0, max = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int i = 18 + (y * w + x) * 3, d = 0;
                for (int c = 0; c < 3; c++) d = Math.Max(d, Math.Abs(a[i + c] - b[i + c]));
                int top = h - 1 - y, o = (top * w + x) * 3;   // TGA rows are bottom-up
                if (img != null) {
                    if (d != 0) { img[o] = 0; img[o + 1] = 0; img[o + 2] = 255; }
                    else { img[o] = (byte)(a[i] / 3); img[o + 1] = (byte)(a[i + 1] / 3); img[o + 2] = (byte)(a[i + 2] / 3); }
                }
                if (d == 0 || top >= maxY) continue;
                if (n != null && (n[i] != a[i] || n[i + 1] != a[i + 1] || n[i + 2] != a[i + 2])) continue;
                any++; if (d > threshold) over++; if (d > max) max = d;
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (top < y0) y0 = top; if (top > y1) y1 = top;
            }
        return new int[] { any, over, max, x0, y0, x1, y1 };
    }
}
'@
if ($DiffDir) { New-Item -ItemType Directory -Force $DiffDir | Out-Null }
foreach ($f in Get-ChildItem $A -Filter *.tga | Sort-Object Name) {
    $other = Join-Path $B $f.Name
    if (-not (Test-Path $other)) { "{0}: missing in B" -f $f.Name; continue }
    $da = [IO.File]::ReadAllBytes($f.FullName); $db = [IO.File]::ReadAllBytes($other)
    if ($da.Length -ne $db.Length) { "{0}: different sizes" -f $f.Name; continue }
    $w = $da[12] + 256 * $da[13]; $h = $da[14] + 256 * $da[15]
    $img = if ($DiffDir) { New-Object byte[] ($w * $h * 3) } else { $null }
    $dn = if ($Noise) { [IO.File]::ReadAllBytes((Join-Path $Noise $f.Name)) } else { $null }
    $r = [TgaDiff]::Compare($da, $db, $Threshold, $img, $MaxY, $dn)
    if ($r[0] -eq 0) { "{0}: identical" -f $f.Name; continue }
    "{0}: {1} differ, {2} by more than {3}, max {4}, box x {5}-{6} y {7}-{8}" -f $f.Name, $r[0], $r[1], $Threshold, $r[2], $r[3], $r[5], $r[4], $r[6]
    if ($DiffDir) {
        $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $data = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $w, $h), [Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
        for ($y = 0; $y -lt $h; $y++) {
            [Runtime.InteropServices.Marshal]::Copy($img, $y * $w * 3, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $w * 3)
        }
        $bmp.UnlockBits($data)
        $bmp.Save((Join-Path $DiffDir ($f.BaseName + '_diff.png')), [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    }
}
