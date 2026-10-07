# HDR output checks (story 7.3; docs/hexenlicht/TESTING.md, "HDR output (7.3)").
#  -A <tga> -B <tga>: how many channel values of two screenshots differ (0 for
#   the same image), their mean and largest difference and the means.
#  -Tga <tga> -Pfm <pfm>: a vk_hdrshot of the same frame as the screenshot,
#   in nits, against the screenshot's 8-bit values: below 255 each value's
#   nits back as an 8-bit value (-White times the value^-Gamma, the sRGB
#   curve with -Srgb), its difference from the screenshot's (within about
#   0.5 for scRGB; HDR10's 10-bit PQ through BT.2020 and back adds up to 1
#   on unsaturated colors, more in a saturated color's near-black channels);
#   at 255 (above SDR's white) the range of nits and how many are above
#   -Peak (none for scRGB).
param([string]$A, [string]$B, [string]$Tga, [string]$Pfm, [double]$White = 300, [double]$Peak = 450,
      [double]$Gamma = 2.2, [switch]$Srgb)
$ErrorActionPreference = 'Stop'
if (-not ('HdrCheck' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Text;
public static class HdrCheck {
	static byte[] Tga(string f, out int w, out int h) {
		byte[] b = File.ReadAllBytes(f); w = b[12] | (b[13] << 8); h = b[14] | (b[15] << 8);
		int bpp = b[16] / 8, data = 18 + b[0]; bool top = (b[17] & 0x20) != 0;
		byte[] rgb = new byte[w * h * 3];
		for (int y = 0; y < h; y++) { int row = top ? y : h - 1 - y;
			for (int x = 0; x < w; x++) { int o = data + (row * w + x) * bpp, p = (y * w + x) * 3;
				rgb[p] = b[o + 2]; rgb[p + 1] = b[o + 1]; rgb[p + 2] = b[o]; } }
		return rgb;	// RGB, rows top-down
	}
	static float[] Pfm(string f, out int w, out int h) {
		byte[] b = File.ReadAllBytes(f); int pos = 0, nl = 0;
		while (nl < 3) { if (b[pos] == '\n') nl++; pos++; }
		string[] hdr = Encoding.ASCII.GetString(b, 0, pos).Split(new[] { '\n', ' ' }, StringSplitOptions.RemoveEmptyEntries);
		w = int.Parse(hdr[1]); h = int.Parse(hdr[2]);
		float[] v = new float[w * h * 3];
		for (int y = 0; y < h; y++) for (int i = 0; i < w * 3; i++)	// PFM's rows go bottom-up
			v[y * w * 3 + i] = BitConverter.ToSingle(b, pos + ((h - 1 - y) * w * 3 + i) * 4);
		return v;
	}
	public static string Diff(string a, string b) {
		int w, h, w2, h2; byte[] p = Tga(a, out w, out h), q = Tga(b, out w2, out h2);
		if (w != w2 || h != h2) return "sizes differ";
		long n = 0, sum = 0, sa = 0, sb = 0; int mx = 0;
		for (int i = 0; i < p.Length; i++) { int d = Math.Abs(p[i] - q[i]); if (d > 0) n++; sum += d; if (d > mx) mx = d; sa += p[i]; sb += q[i]; }
		return String.Format("{0} of {1} values differ, mean |d| {2:F3}, max {3}; means {4:F2} {5:F2}",
			n, p.Length, (double)sum / p.Length, mx, (double)sa / p.Length, (double)sb / p.Length);
	}
	public static string Nits(string tga, string pfm, double white, double peak, double gamma, bool srgb) {
		int w, h, w2, h2; byte[] p = Tga(tga, out w, out h); float[] v = Pfm(pfm, out w2, out h2);
		if (w != w2 || h != h2) return "sizes differ";
		double maxCode = 0, sumCode = 0, maxNits = 0, minClipped = double.MaxValue; long n = 0, clipped = 0, over = 0;
		for (int i = 0; i < p.Length; i++) {
			double nits = v[i]; if (nits > maxNits) maxNits = nits;
			if (p[i] == 255) { clipped++; if (nits < minClipped) minClipped = nits; if (nits > peak * 1.001) over++; continue; }
			double lin = Math.Max(nits, 0) / white;
			double code = srgb ? (lin <= 0.0031308 ? lin * 12.92 : 1.055 * Math.Pow(lin, 1 / 2.4) - 0.055) : Math.Pow(lin, 1 / gamma);
			double e = Math.Abs(code * 255 - p[i]); sumCode += e; n++; if (e > maxCode) maxCode = e;
		}
		return String.Format("below white {0} values, |d| mean {1:F3} max {2:F3}; at white {3} values, {4:F1}-{5:F1} nits, {6} above the peak",
			n, sumCode / Math.Max(n, 1), maxCode, clipped, clipped > 0 ? minClipped : 0, maxNits, over);
	}
}
'@
}
if ($A) { "{0} vs {1}: {2}" -f (Split-Path $A -Leaf), (Split-Path $B -Leaf), [HdrCheck]::Diff($A, $B) }
if ($Tga) { "{0} vs {1}: {2}" -f (Split-Path $Tga -Leaf), (Split-Path $Pfm -Leaf), [HdrCheck]::Nits($Tga, $Pfm, $White, $Peak, $Gamma, [bool]$Srgb) }
