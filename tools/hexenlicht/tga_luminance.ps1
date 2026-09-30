param([string[]]$Files)
# The mean luminance in linear light (sRGB decoded, Rec. 709 weights) of the
# engine's uncompressed TGA screenshots, one line each: for comparing whole
# views, such as the test pack's albedo question (story 5.6, TESTING.md "Test
# pack"; tga_mean.ps1 averages numbered shots of one frame and compares two
# sets).
if (-not ('TgaLuminance' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class TgaLuminance {
	public static double Mean(string f) {
		var b = File.ReadAllBytes(f);
		int idl = b[0], type = b[2], w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bpp = b[16] / 8;
		if (type != 2 || bpp < 3) throw new Exception(f + ": not an uncompressed 24- or 32-bit TGA");
		var lut = new double[256];
		for (int i = 0; i < 256; i++) { double v = i / 255.0; lut[i] = v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
		int o = 18 + idl; double s = 0;
		for (int i = 0; i < w * h; i++, o += bpp) s += 0.2126 * lut[b[o + 2]] + 0.7152 * lut[b[o + 1]] + 0.0722 * lut[b[o]];
		return s / (w * h);
	}
}
'@
}
foreach ($f in $Files) {
	$full = (Resolve-Path -LiteralPath $f).ProviderPath	# .NET resolves relative paths against the process's folder
	"{0,-40} {1:N4}" -f (Split-Path $full -Leaf), [TgaLuminance]::Mean($full)
}
