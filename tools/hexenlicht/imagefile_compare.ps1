param([string]$Shots = '', [string]$Data = '')

# Compares the screenshots of imagefile_set.ps1's test run (story 5.2;
# docs/hexenlicht/TESTING.md, "Image files") with what the files hold: each
# picture sits at the shot's top left, one screen pixel per texel or a
# quarter of that. Per shot (imgtest\shots.txt: file, scale, mode,
# reference, check) the mean and largest channel difference against its
# reference in the set: the generated source, texconv's decode of a lossy or
# converted file (the loader must show what the file holds), or the flat
# color of a mip level. Modes: grey (the reference's red in R, G and B),
# alpha (the picture over black: color x alpha), rg (BC5: red and green,
# blue 0). Checks, from what 5.2 measured: exact (equal), decode (within 4,
# mean 0.1: the GPU's BC7 decode differed from DirectXTex's in one 4x4
# block), flat (a quarter-size shot showing one level's color, within 2),
# box (a quarter against the reference averaged over 4x4 texels: made mips,
# within 2). CHECK marks a shot outside.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$set = Join-Path $Data 'data1\textures\imgtest'
if (-not $Shots) { $Shots = Join-Path $Data 'data1\shots' }

if (-not ('ImageFileCompare' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class ImageFileCompare {
	// an uncompressed 24- or 32-bit TGA as RGBA rows, top first
	public static byte[] Load(string f, out int w, out int h) {
		var b = File.ReadAllBytes(f); w = b[12] | (b[13] << 8); h = b[14] | (b[15] << 8);
		int bpp = b[16] / 8, ofs = 18 + b[0]; bool top = (b[17] & 0x20) != 0;
		if (b[2] != 2 || (bpp != 3 && bpp != 4)) throw new Exception(f + ": not an uncompressed 24/32-bit TGA");
		var o = new byte[w * h * 4];
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			int sy = top ? y : h - 1 - y, s = ofs + (sy * w + x) * bpp, d = (y * w + x) * 4;
			o[d] = b[s + 2]; o[d + 1] = b[s + 1]; o[d + 2] = b[s]; o[d + 3] = bpp == 4 ? b[s + 3] : (byte)255;
		}
		return o;
	}
	// mean and largest difference over the reference's texels (div 4: averaged over 4x4)
	public static double[] Compare(string shot, string reference, string mode, int div) {
		int sw, sh, w, h; var s = Load(shot, out sw, out sh); var r = Load(reference, out w, out h);
		int ow = w / div, oh = h / div, max = 0, n = 0; double sum = 0;
		for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) for (int c = 0; c < 3; c++) {
			double e = 0;
			for (int j = 0; j < div; j++) for (int i = 0; i < div; i++) {
				int p = ((y * div + j) * w + x * div + i) * 4;
				if (mode == "grey") e += r[p];
				else if (mode == "alpha") e += Math.Round(r[p + c] * r[p + 3] / 255.0);
				else if (mode == "rg" && c == 2) e += 0;
				else e += r[p + c];
			}
			int d = (int)Math.Abs(Math.Round(e / (div * div)) - s[(y * sw + x) * 4 + c]);
			sum += d; n++; if (d > max) max = d;
		}
		return new double[] { sum / n, max };
	}
}
'@
}

$checks = 0
foreach ($l in Get-Content (Join-Path $set 'shots.txt')) {
	$n, $f, $scale, $mode, $reference, $check = $l -split ' '
	$div = if ($check -eq 'box') { 4 } else { 1 }
	$shot = Join-Path $Shots ('hexen{0:D2}.tga' -f [int]$n)
	$d = [ImageFileCompare]::Compare($shot, (Join-Path $set $reference), $mode, $div)
	$bad = switch ($check) {
		'exact' { $d[1] -gt 0 }
		'decode' { $d[1] -gt 4 -or $d[0] -gt 0.1 }
		default { $d[1] -gt 2 }
	}
	$line = '{0,2} {1,-18} x{2,-5} {3,-5} {4,-6} {5,-22} mean {6,5:F2} max {7,3}' -f $n, $f, $scale, $mode, $check, $reference, $d[0], $d[1]
	if ($bad) { $line += '   CHECK'; $checks++ }
	$line
}
if ($checks) { "$checks shots outside 5.2's measurements" } else { 'all shots as 5.2 measured' }
