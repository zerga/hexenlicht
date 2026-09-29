# Compares calib_shots.ps1's matched shots of glh2 and Hexenlicht (story 4.9;
# docs/hexenlicht/TESTING.md, "Calibration against GL").
#  - The light: GL's lightmaps (<name>_gllm.tga, r_lightmap 1) against
#    Hexenlicht's direct diffuse light (<name>_direct.tga, divided by the
#    shots' -Scale), both in linear light: GL multiplied its textures' 8-bit
#    colors by the lightmap's, so its light in linear light is the lightmap
#    decoded (by -Transfer, below: the 2.2 power since 4.17). Pixels count
#    where GL's is grey (a lightmap: not the sky, liquids or other unlit
#    surfaces), neither is clipped nor black; blocks of
#    -Block pixels with at least half of them. Per bookmark: the blocks, the
#    median of Hexenlicht / GL (the scale to match: 1 = GL's units), the
#    spread (the quartiles' distance, in stops) and the slope of log
#    Hexenlicht over log GL (1: the same contrast; above: Hexenlicht's is
#    higher); and all bookmarks pooled.
#  - The look: GL's image (<name>_glc.tga with HoT's colored light when
#    calib_shots.ps1 -GlLit made it, else <name>_gl.tga) against Hexenlicht's lit image
#    (<name>_lit.tga): the mean luminance (linear) of each and their ratio;
#    "surfaces": the median ratio of blocks on the lightmapped world only
#    (as the light's mask, so no sky), per bookmark and pooled (the
#    number the map lights' scale is divided by to match GL's look:
#    r_maplight_fit_scale, 4.16; with the physical shapes r_maplight_scale);
#    "+clip" (4.16): the same with the blocks where GL's lightmap is
#    clipped (the light's mask leaves them out: the fill-lit rooms and
#    yards, where GL's sum of lights reached a full texel), and "spread"
#    those blocks' spread in stops (how evenly the look matches). -White
#    compares with <name>_gl.tga even where a colored one exists.
#  - -Pictures: <Out>\<label>\compare\<name>.png, half size: GL | Hexenlicht |
#    Hexenlicht / GL (blue darker, red brighter, to 2 stops; black not
#    counted) for the image (top) and the light (bottom).
# -Labels: the subfolders of Hexenlicht shots to compare (calib_shots.ps1's
# -Label), each against the same GL shots; -Markdown writes the tables there.
# -Transfer: how the shots' 8-bit colors are linear light, the engine's (4.17):
# 2.2 (a power, the default; GL's lightmap texels are the fit's power of it
# too), srgb for shots of a build before 4.17 or with r_srgb 1.
# After the pooled row (4.11, TESTING.md's "Calibrating a hub"): how many views'
# "+clip" look is within -Range (0.85 to 1.2) and the mean error per view in
# stops (views without such blocks left out and counted); -ByMap adds a pooled
# row per map, the map being a bookmark's name up to its first _.
param([Parameter(Mandatory)][string]$Out, [string[]]$Labels = @('hl'), [double]$Scale = 0.25, [int]$Block = 30,
      [switch]$Pictures, [switch]$White, [string]$Markdown = '', [string]$Transfer = '2.2', [switch]$ByMap,
      [ValidateCount(2, 2)][double[]]$Range = @(0.85, 1.2))
$ErrorActionPreference = 'Stop'
if ($Block -lt 4) { throw '-Block: at least 4 pixels' }
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Collections.Generic;
public static class Calib {
    // the shots' 8-bit colors as linear light: a power gamma, 0 = the sRGB curve (4.17)
    static double gamma = 2.2;
    static double[] lut = MakeLut();
    static double[] MakeLut() {
        var t = new double[256];
        for (int i = 0; i < 256; i++) { double v = i / 255.0; t[i] = gamma > 0 ? Math.Pow(v, gamma) : v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
        return t;
    }
    public static void SetTransfer(double g) { gamma = g; lut = MakeLut(); }
    static byte Encode(double x) {
        x = Math.Min(Math.Max(x, 0), 1);
        x = gamma > 0 ? Math.Pow(x, 1 / gamma) : x <= 0.0031308 ? x * 12.92 : 1.055 * Math.Pow(x, 1 / 2.4) - 0.055;
        return (byte)(x * 255 + 0.5);
    }
    // an uncompressed 24-bit TGA as top-down RGB
    public static byte[] Read(string f, out int w, out int h) {
        var b = File.ReadAllBytes(f);
        w = b[12] | (b[13] << 8); h = b[14] | (b[15] << 8);
        int ofs = 18 + b[0]; bool top = (b[17] & 0x20) != 0;
        if (b[2] != 2 || b[16] != 24) throw new Exception(f + ": not an uncompressed 24-bit TGA");
        var o = new byte[w * h * 3];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++)
                    o[(y * w + x) * 3 + c] = b[ofs + ((top ? y : h - 1 - y) * w + x) * 3 + (2 - c)];
        return o;
    }
    // the same size as the first image, or an exception
    static byte[] ReadAs(string f, int w, int h) {
        int w2, h2; var p = Read(f, out w2, out h2);
        if (w2 != w || h2 != h) throw new Exception(f + ": not the size of the other shots");
        return p;
    }
    static double Lum(byte[] p, int i) { return 0.2126 * lut[p[i]] + 0.7152 * lut[p[i + 1]] + 0.0722 * lut[p[i + 2]]; }
    static bool Grey(byte[] p, int i) { return Math.Abs(p[i] - p[i + 1]) <= 2 && Math.Abs(p[i + 1] - p[i + 2]) <= 2; }
    static bool Counted(byte[] g, byte[] e, int i) {
        return Grey(g, i) && g[i + 1] >= 12 && g[i + 1] <= 250 && e[i + 1] >= 3 && e[i + 1] <= 252;
    }
    // the light's blocks: log2 of GL's and Hexenlicht's means
    public static void Blocks(string gllm, string direct, double scale, int block, List<double> lg, List<double> le) {
        int w, h, w2, h2;
        byte[] g = Read(gllm, out w, out h), e = Read(direct, out w2, out h2);
        if (w != w2 || h != h2) throw new Exception(direct + ": not the size of " + gllm);
        for (int by = 0; by + block <= h; by += block)
            for (int bx = 0; bx + block <= w; bx += block) {
                double sg = 0, se = 0; int n = 0;
                for (int y = by; y < by + block; y++)
                    for (int x = bx; x < bx + block; x++) {
                        int i = (y * w + x) * 3;
                        if (!Counted(g, e, i)) continue;
                        sg += Lum(g, i); se += Lum(e, i) / scale; n++;
                    }
                if (n * 2 < block * block) continue;
                lg.Add(Math.Log(sg / n, 2)); le.Add(Math.Log(se / n, 2));
            }
    }
    static double Quantile(List<double> s, double q) {
        if (s.Count == 0) return double.NaN;
        double p = q * (s.Count - 1); int i = (int)Math.Floor(p);
        return i + 1 < s.Count ? s[i] + (p - i) * (s[i + 1] - s[i]) : s[i];
    }
    // blocks, median ratio, spread (stops), slope
    public static double[] Stats(List<double> lg, List<double> le) {
        var r = new List<double>();
        double mg = 0, me = 0, sxy = 0, sxx = 0;
        for (int i = 0; i < lg.Count; i++) { r.Add(le[i] - lg[i]); mg += lg[i]; me += le[i]; }
        if (lg.Count == 0) return new double[] { 0, double.NaN, double.NaN, double.NaN };
        mg /= lg.Count; me /= lg.Count;
        for (int i = 0; i < lg.Count; i++) { sxy += (lg[i] - mg) * (le[i] - me); sxx += (lg[i] - mg) * (lg[i] - mg); }
        r.Sort();
        return new double[] { lg.Count, Math.Pow(2, Quantile(r, 0.5)), Quantile(r, 0.75) - Quantile(r, 0.25), sxx > 0 ? sxy / sxx : double.NaN };
    }
    // the look on the lightmapped world only (GL's lightmap shot grey, not
    // black, and not clipped unless clipped; the GL image not black): log2
    // of Hexenlicht's lit over GL's image, per block
    public static void SurfaceBlocks(string gl, string lit, string gllm, int block, List<double> r, bool clipped) {
        int w, h;
        byte[] a = Read(gl, out w, out h), b = ReadAs(lit, w, h), m = ReadAs(gllm, w, h);
        for (int by = 0; by + block <= h; by += block)
            for (int bx = 0; bx + block <= w; bx += block) {
                double sa = 0, sb = 0; int n = 0;
                for (int y = by; y < by + block; y++)
                    for (int x = bx; x < bx + block; x++) {
                        int i = (y * w + x) * 3;
                        if (!Grey(m, i) || m[i + 1] < 12 || (m[i + 1] > 250 && !clipped) || Lum(a, i) < 0.002) continue;
                        sa += Lum(a, i); sb += Lum(b, i); n++;
                    }
                if (n * 2 >= block * block && sa > 0 && sb > 0) r.Add(Math.Log(sb / sa, 2));
            }
    }
    // the quartiles' distance of log2 ratios, in stops
    public static double Spread(List<double> v) {
        var s = new List<double>(v); s.Sort();
        return s.Count > 0 ? Quantile(s, 0.75) - Quantile(s, 0.25) : double.NaN;
    }
    public static double Median(List<double> v) {
        var s = new List<double>(v); s.Sort();
        return s.Count > 0 ? Math.Pow(2, Quantile(s, 0.5)) : double.NaN;
    }
    public static double MeanLum(string f) {
        int w, h; var p = Read(f, out w, out h); double s = 0;
        for (int i = 0; i < p.Length; i += 3) s += Lum(p, i);
        return s / (w * h);
    }
    // a heat pixel of log2 ratio r (blue below, red above, 2 stops full)
    static void Heat(byte[] o, int j, double r) {
        double t = Math.Min(Math.Abs(r) / 2, 1);
        byte hi = 255, lo = (byte)(255 * (1 - t));
        if (r >= 0) { o[j] = hi; o[j + 1] = lo; o[j + 2] = lo; } else { o[j] = lo; o[j + 1] = lo; o[j + 2] = hi; }
    }
    // the 3 x 2 picture at half size as a bottom-up 24-bit TGA
    public static void Picture(string gl, string lit, string gllm, string direct, double scale, string outTga) {
        int w, h;
        byte[] a = Read(gl, out w, out h), b = ReadAs(lit, w, h), c = ReadAs(gllm, w, h), d = ReadAs(direct, w, h);
        int tw = w / 2, th = h / 2, W = tw * 3, H = th * 2;
        var o = new byte[W * H * 3];
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                double[] s = new double[12]; int nc = 0; double rl = 0, rd = 0;	// a b c d sums (RGB), counted light pixels
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int i = ((2 * y + dy) * w + 2 * x + dx) * 3;
                        for (int k = 0; k < 3; k++) {
                            s[k] += lut[a[i + k]]; s[3 + k] += lut[b[i + k]]; s[6 + k] += lut[c[i + k]]; s[9 + k] += lut[d[i + k]] / scale;
                        }
                        rl += Math.Log(Math.Max(Lum(b, i), 1e-4) / Math.Max(Lum(a, i), 1e-4), 2);
                        if (Counted(c, d, i)) { rd += Math.Log((Lum(d, i) / scale) / Lum(c, i), 2); nc++; }
                    }
                int[] tx = { 0, tw, 0, tw }, ty = { 0, 0, th, th };
                for (int t = 0; t < 4; t++) {
                    int j = (((ty[t] + y) * W) + tx[t] + x) * 3;
                    for (int k = 0; k < 3; k++) o[j + k] = Encode(s[t * 3 + k] / 4);
                }
                Heat(o, ((y * W) + 2 * tw + x) * 3, rl / 4);
                int jd = (((th + y) * W) + 2 * tw + x) * 3;
                if (nc > 0) Heat(o, jd, rd / nc); else { o[jd] = o[jd + 1] = o[jd + 2] = 0; }
            }
        var f = new byte[18 + o.Length];
        f[2] = 2; f[12] = (byte)(W & 255); f[13] = (byte)(W >> 8); f[14] = (byte)(H & 255); f[15] = (byte)(H >> 8); f[16] = 24;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                for (int k = 0; k < 3; k++)
                    f[18 + ((H - 1 - y) * W + x) * 3 + (2 - k)] = o[(y * W + x) * 3 + k];
        File.WriteAllBytes(outTga, f);
    }
}
'@
[Calib]::SetTransfer($(if ($Transfer -eq 'srgb') { 0.0 } else { [double]::Parse($Transfer, [Globalization.CultureInfo]::InvariantCulture) }))

$inv = [Globalization.CultureInfo]::InvariantCulture
$gldir = Join-Path $Out 'gl'
$names = @(Get-ChildItem $gldir -Filter '*_gllm.tga' | ForEach-Object { $_.Name -replace '_gllm\.tga$', '' } | Sort-Object)
if (-not $names) { throw "no GL shots in $gldir" }
$md = [Collections.Generic.List[string]]::new()
function F([double]$v, [string]$f) { $v.ToString($f, $inv) }
$fmt = '{0,-18} {1,6} {2,8} {3,7} {4,6}  {5,8} {6,8} {7,8} {8,9} {9,7} {10,7}{11}'
foreach ($label in $Labels) {
	$hldir = Join-Path $Out $label
	$pg = [Collections.Generic.List[double]]::new(); $pe = [Collections.Generic.List[double]]::new()
	$ps = [Collections.Generic.List[double]]::new(); $pc = [Collections.Generic.List[double]]::new(); $ratios = [Collections.Generic.List[double]]::new()
	$maps = [ordered]@{}; $views = [Collections.Generic.List[double]]::new()
	"== ${label}: the light (direct / GL lightmap) and the look (lit / GL image)"
	$fmt -f 'bookmark', 'blocks', 'median', 'spread', 'slope', 'GL mean', 'HL mean', 'ratio', 'surfaces', '+clip', 'spread', ''
	$md.Add("### $label"); $md.Add('')
	$md.Add('| bookmark | blocks | light median | spread (stops) | slope | GL mean | Hexenlicht mean | ratio | on surfaces | with the clipped | their spread (stops) |')
	$md.Add('|---|---|---|---|---|---|---|---|---|---|---|')
	foreach ($n in $names) {
		$direct = Join-Path $hldir "${n}_direct.tga"; $lit = Join-Path $hldir "${n}_lit.tga"
		if (-not (Test-Path $direct) -or -not (Test-Path $lit)) { continue }
		$gllm = Join-Path $gldir "${n}_gllm.tga"
		$lg = [Collections.Generic.List[double]]::new(); $le = [Collections.Generic.List[double]]::new()
		[Calib]::Blocks($gllm, $direct, $Scale, $Block, $lg, $le)
		$pg.AddRange($lg); $pe.AddRange($le)
		$s = [Calib]::Stats($lg, $le)
		$glimg = Join-Path $gldir "${n}_glc.tga"; $colored = (-not $White) -and (Test-Path $glimg)
		if (-not $colored) { $glimg = Join-Path $gldir "${n}_gl.tga" }
		$g = [Calib]::MeanLum($glimg); $h = [Calib]::MeanLum($lit)
		$sb = [Collections.Generic.List[double]]::new()
		[Calib]::SurfaceBlocks($glimg, $lit, $gllm, $Block, $sb, $false)
		$sc = [Collections.Generic.List[double]]::new()
		[Calib]::SurfaceBlocks($glimg, $lit, $gllm, $Block, $sc, $true)
		$pc.AddRange($sc)
		$views.Add([Calib]::Median($sc))
		$m = ($n -split '_')[0]
		if (-not $maps.Contains($m)) { $maps[$m] = @{ g = [Collections.Generic.List[double]]::new(); e = [Collections.Generic.List[double]]::new(); s = [Collections.Generic.List[double]]::new(); c = [Collections.Generic.List[double]]::new(); r = [Collections.Generic.List[double]]::new() } }
		$maps[$m].g.AddRange($lg); $maps[$m].e.AddRange($le); $maps[$m].s.AddRange($sb); $maps[$m].c.AddRange($sc)
		$ps.AddRange($sb); $ratios.Add([Math]::Log($h / $g, 2))
		$maps[$m].r.Add([Math]::Log($h / $g, 2))
		$sm = [Calib]::Median($sb)
		$fmt -f $n, $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'), (F $s[3] 'F2'), (F $g 'F4'), (F $h 'F4'), (F ($h / $g) 'F2'),
			(F $sm 'F2'), (F ([Calib]::Median($sc)) 'F2'), (F ([Calib]::Spread($sc)) 'F2'), $(if ($colored) { '  (GL with HoT colors)' } else { '' })
		$md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} |' -f $n, $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'),
			(F $s[3] 'F2'), (F $g 'F4'), (F $h 'F4'), (F ($h / $g) 'F2'), (F $sm 'F2'), (F ([Calib]::Median($sc)) 'F2'), (F ([Calib]::Spread($sc)) 'F2')))
		if ($Pictures) {
			$cmp = Join-Path $hldir 'compare'; New-Item -ItemType Directory -Force $cmp | Out-Null
			$tga = Join-Path $cmp "$n.tga"
			[Calib]::Picture($glimg, $lit, $gllm, $direct, $Scale, $tga)
			& (Join-Path $PSScriptRoot 'tga2png.ps1') -Files $tga -OutDir $cmp | Out-Null
			[IO.File]::Delete($tga)
		}
	}
	$s = [Calib]::Stats($pg, $pe)
	$fmt -f 'all', $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'), (F $s[3] 'F2'), '', '', (F ([Calib]::Median($ratios)) 'F2'), (F ([Calib]::Median($ps)) 'F2'), (F ([Calib]::Median($pc)) 'F2'), (F ([Calib]::Spread($pc)) 'F2'), ''
	$md.Add(('| **all** | {0} | {1} | {2} | {3} | | | {4} | {5} | {6} | {7} |' -f $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'), (F $s[3] 'F2'),
		(F ([Calib]::Median($ratios)) 'F2'), (F ([Calib]::Median($ps)) 'F2'), (F ([Calib]::Median($pc)) 'F2'), (F ([Calib]::Spread($pc)) 'F2')))
	# the views' look with the clipped blocks within -Range, and the mean error per view (4.11)
	$valid = @($views | Where-Object { -not [double]::IsNaN($_) })
	$in = @($valid | Where-Object { $_ -ge $Range[0] -and $_ -le $Range[1] }).Count
	$err = if ($valid.Count) { ($valid | ForEach-Object { [Math]::Abs([Math]::Log($_, 2)) } | Measure-Object -Average).Average } else { 0 }
	$line = 'views with the look (+clip) within {0}-{1}: {2} of {3}; the mean error per view {4} stops{5}' -f (F $Range[0] 'F2'), (F $Range[1] 'F2'), $in, $valid.Count, (F $err 'F2'),
		$(if ($valid.Count -ne $views.Count) { ' (' + ($views.Count - $valid.Count) + ' more without blocks)' } else { '' })
	$line; $md.Add(''); $md.Add($line)
	if ($ByMap) {
		# pooled per map (the bookmark's name up to its first _)
		$md.Add(''); $md.Add('| map | blocks | light median | spread (stops) | slope | ratio | on surfaces | with the clipped | their spread (stops) |'); $md.Add('|---|---|---|---|---|---|---|---|---|')
		foreach ($m in $maps.Keys) {
			$v = $maps[$m]; $s = [Calib]::Stats($v.g, $v.e)
			$fmt -f "map $m", $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'), (F $s[3] 'F2'), '', '', (F ([Calib]::Median($v.r)) 'F2'), (F ([Calib]::Median($v.s)) 'F2'), (F ([Calib]::Median($v.c)) 'F2'), (F ([Calib]::Spread($v.c)) 'F2'), ''
			$md.Add(('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} |' -f $m, $s[0], (F $s[1] 'F3'), (F $s[2] 'F2'), (F $s[3] 'F2'),
				(F ([Calib]::Median($v.r)) 'F2'), (F ([Calib]::Median($v.s)) 'F2'), (F ([Calib]::Median($v.c)) 'F2'), (F ([Calib]::Spread($v.c)) 'F2')))
		}
	}
	$md.Add('')
	''
}
if ($Markdown) { Set-Content -Path $Markdown -Value ($md -join "`n") }
