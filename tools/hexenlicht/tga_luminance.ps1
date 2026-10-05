param([string[]]$Files, [int[]]$Rect = @())
# The mean luminance in linear light (sRGB decoded, Rec. 709 weights) of the
# engine's uncompressed TGA screenshots, one line each: for comparing whole
# views, such as the test pack's albedo question (story 5.6, TESTING.md "Test
# pack"; tga_mean.ps1 averages numbered shots of one frame and compares two
# sets). -Rect x,y,w,h: of that rectangle only (x right, y down from the top
# left), e.g. the view's centre over consecutive frames (6.18, TESTING.md
# "Water (6.5)").
if (-not ('TgaRectLuminance' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class TgaRectLuminance {
	public static double Mean(string f, int x0, int y0, int rw, int rh) {
		var b = File.ReadAllBytes(f);
		int idl = b[0], type = b[2], w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bpp = b[16] / 8;
		bool top = (b[17] & 0x20) != 0;
		if (type != 2 || bpp < 3) throw new Exception(f + ": not an uncompressed 24- or 32-bit TGA");
		if (rw <= 0 || rh <= 0) { x0 = 0; y0 = 0; rw = w; rh = h; }
		if (x0 < 0 || y0 < 0 || x0 + rw > w || y0 + rh > h) throw new Exception(f + ": the rectangle is outside the image");
		var lut = new double[256];
		for (int i = 0; i < 256; i++) { double v = i / 255.0; lut[i] = v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
		double s = 0;
		for (int y = y0; y < y0 + rh; y++) {
			int o = 18 + idl + ((top ? y : h - 1 - y) * w + x0) * bpp;
			for (int x = 0; x < rw; x++, o += bpp) s += 0.2126 * lut[b[o + 2]] + 0.7152 * lut[b[o + 1]] + 0.0722 * lut[b[o]];
		}
		return s / ((double)rw * rh);
	}
}
'@
}
if ($Rect.Count -ne 0 -and $Rect.Count -ne 4) { throw '-Rect takes x,y,w,h' }
$r = if ($Rect.Count -eq 4) { $Rect } else { @(0, 0, 0, 0) }
foreach ($f in $Files) {
	$full = (Resolve-Path -LiteralPath $f).ProviderPath	# .NET resolves relative paths against the process's folder
	"{0,-40} {1:N4}" -f (Split-Path $full -Leaf), [TgaRectLuminance]::Mean($full, $r[0], $r[1], $r[2], $r[3])
}
