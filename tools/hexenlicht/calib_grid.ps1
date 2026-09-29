# A labelled grid of calib_shots.ps1's shots, for looking at a hub's views side
# by side (story 4.11; docs/hexenlicht/TESTING.md, "Calibrating a hub"): a row
# per bookmark (its name above it), a column per -Columns entry
# 'Title|pattern|scale': the pattern is a path under -Out with {0} for the
# bookmark's name, the scale multiplies the shot in linear light (4: a
# direct-light shot at r_debugview_scale 0.25 as it is). Tiles are the shots at
# -Half of their size; the grid is written to -Png. Examples:
#   -Columns 'GL|gl\{0}_gl.tga|1', 'Hexenlicht|hl\{0}_lit.tga|1'
#   -Columns 'GL lightmaps|gl\{0}_gllm.tga|1', 'direct x4|hl\{0}_direct.tga|4'
# For the scaling a shot's 8-bit colors are linear light by -Transfer, the
# engine's curve (4.17): 2.2 (a power, the default), srgb for older shots; the
# scale 1 copies them as they are.
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string[]]$Names, [Parameter(Mandatory)][string]$Png,
      [Parameter(Mandatory)][string[]]$Columns, [double]$Half = 0.5, [string]$Transfer = '2.2')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('GridTga2' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class GridTga2 {
    // the transfer: a power gamma, 0 = the sRGB curve (as calib_compare.ps1's)
    public static double Gamma = 2.2;
    static double Dec(double v) { v /= 255.0; return Gamma > 0 ? Math.Pow(v, Gamma) : v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
    static byte Enc(double x) { x = Math.Min(Math.Max(x, 0), 1); x = Gamma > 0 ? Math.Pow(x, 1 / Gamma) : x <= 0.0031308 ? x * 12.92 : 1.055 * Math.Pow(x, 1 / 2.4) - 0.055; return (byte)(x * 255 + 0.5); }
    // an uncompressed 24-bit TGA as top-down BGR rows, scaled in linear light
    public static byte[] Load(string f, double scale, out int w, out int h) {
        var b = File.ReadAllBytes(f);
        if (b[2] != 2 || b[16] != 24) throw new Exception(f + ": not an uncompressed 24-bit TGA");
        w = b[12] | (b[13] << 8); h = b[14] | (b[15] << 8); int ofs = 18 + b[0]; bool top = (b[17] & 0x20) != 0;
        var lut = new byte[256];
        for (int i = 0; i < 256; i++) lut[i] = scale == 1 ? (byte)i : Enc(Dec(i) * scale);
        var o = new byte[w * h * 3];
        for (int y = 0; y < h; y++) {
            int src = ofs + (top ? y : h - 1 - y) * w * 3;
            for (int x = 0; x < w * 3; x++) o[y * w * 3 + x] = lut[b[src + x]];
        }
        return o;
    }
}
'@
}

function Load-Bitmap([string]$f, [double]$s) {
	$w = 0; $h = 0; $px = [GridTga2]::Load($f, $s, [ref]$w, [ref]$h)
	$bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
	$d = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, $w, $h), [Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
	for ($y = 0; $y -lt $h; $y++) { [Runtime.InteropServices.Marshal]::Copy($px, $y * $w * 3, [IntPtr]($d.Scan0.ToInt64() + $y * $d.Stride), $w * 3) }
	$bmp.UnlockBits($d)
	$bmp
}

$inv = [Globalization.CultureInfo]::InvariantCulture
[GridTga2]::Gamma = if ($Transfer -eq 'srgb') { 0.0 } else { [double]::Parse($Transfer, $inv) }
$cols = @(foreach ($c in $Columns) {
	$p = $c.Split('|')
	if ($p.Count -ne 3) { throw "a column is 'Title|pattern|scale': $c" }
	, @($p[0], $p[1], [double]::Parse($p[2], $inv))
})
$first = Load-Bitmap (Join-Path $Out ($cols[0][1] -f $Names[0])) 1
$cw = [int]($first.Width * $Half); $ch = [int]($first.Height * $Half); $first.Dispose()
$head = 28; $lab = 22
$grid = New-Object Drawing.Bitmap ($cw * $cols.Count), ($head + $Names.Count * ($ch + $lab))
$gr = [Drawing.Graphics]::FromImage($grid)
$gr.Clear([Drawing.Color]::Black)
$gr.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object Drawing.Font 'Segoe UI', 12
for ($c = 0; $c -lt $cols.Count; $c++) { $gr.DrawString($cols[$c][0], $font, [Drawing.Brushes]::White, $c * $cw + 6, 4) }
for ($r = 0; $r -lt $Names.Count; $r++) {
	$y = $head + $r * ($ch + $lab)
	$gr.DrawString($Names[$r], $font, [Drawing.Brushes]::White, 6, $y)
	for ($c = 0; $c -lt $cols.Count; $c++) {
		$bmp = Load-Bitmap (Join-Path $Out ($cols[$c][1] -f $Names[$r])) $cols[$c][2]
		$gr.DrawImage($bmp, $c * $cw, $y + $lab, $cw, $ch)
		$bmp.Dispose()
	}
}
$gr.Dispose(); $font.Dispose()
$grid.Save($Png, [Drawing.Imaging.ImageFormat]::Png)
"$Png $($grid.Width)x$($grid.Height)"
$grid.Dispose()
