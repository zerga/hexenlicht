param([string]$Texconv = '', [string]$Data = '', [string]$Export = '', [ValidateSet('physical', 'matched')][string]$Albedo = 'physical',
      [int]$Scale = 4, [switch]$Dds, [string[]]$LoadSet = @(), [switch]$Run, [switch]$Release, [switch]$Remove,
      [int]$Width = 1280, [int]$Height = 720, [int]$Timeout = 900)

# The test material pack (story 5.6, docs/hexenlicht/TESTING.md "Test pack",
# docs/hexenlicht/AUTHORING.md): generated materials (no game data) under
# the material spec's names (docs/hexenlicht/MATERIALS.md) in data1\textures,
# each from one height field so that its maps agree, at -Scale times the
# original's size (4; its aspect ratio), with measured real-world values:
#  - stone: demo1's walls rtex022 (ashlar blocks), its cobbles rtex005 and
#    the start's brick floor rtex429: albedo, _n, _orm (roughness 0.75-1,
#    not metallic);
#  - metal: the riveted plates rtex011 and rtex054: iron (metallic 1, its
#    reflectance as the albedo, roughness 0.3-0.5 with streaks), rust
#    patches (not metallic, rough);
#  - water: the pool #rtex346 and the blue #rtex078: ripples (_n), .mat
#    roughness 0.05 and specular 0.5 (water's 2 % head-on); the originals'
#    colors;
#  - emissive: runes cut into the stone panel rtex426 (demo1's statue's
#    pedestal and a frieze), _e and .mat emissive 0.15;
#  - glass (.mat kind glass): the stained glass rtex018 (breakable panes;
#    diamond quarries of colored glass in lead), the skylight panes rtex083
#    (clear glass in a lead grid), the clear glass rtex199 (village1-3,
#    keep1-2, 80 world faces in castle5).
# Albedo and emissive are written in the engine's 8-bit transfer (the 2.2
# power, r_srgb 0), so the linear values are the ones intended. -Albedo
# matched scales each stone albedo so its mean luminance in linear light is
# the original's (from -Export, r_exporttextures' folder), keeping the
# pattern (DECISIONS M3's question); the metals keep their reflectance.
# -LoadSet demo1,cath instead writes one stone material (albedo, _n, _orm)
# for every world texture those maps use (the -Export manifest's rows),
# for map load times (PNG against -Dds) and the albedo question over whole
# rooms. -Dds converts the PNGs with pack_dds.ps1 (BC7, BC5, mips), as a
# shipped pack. The images are TGAs made here, converted by Microsoft's
# texconv (DirectXTex, -Texconv, not in the repository) as the other test
# sets. Refuses to run if data1\textures has files it didn't write; lists
# what it wrote in data1\packtest_files.txt; -Remove deletes those, the
# scripts and the sources (data1\packtest_src).
# -Run runs packtest_a.cfg (after the pack) or loadtest_a.cfg (after a
# -LoadSet) with hl_run.ps1 (-Release) at -Width x -Height.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
# full paths: the C# and .NET calls resolve relative ones against the process's folder, not PowerShell's
$Data = (Resolve-Path -LiteralPath $Data).ProviderPath
if ($Export) { $Export = (Resolve-Path -LiteralPath $Export).ProviderPath }
$LoadSet = @($LoadSet | ForEach-Object { $_ -split ',' } | Where-Object { $_ })	# pwsh -File passes "demo1,cath" as one string
$game = Join-Path $Data 'data1'
$tex = Join-Path $game 'textures'
$src = Join-Path $game 'packtest_src'
$manifest = Join-Path $game 'packtest_files.txt'
$scripts = 'packtest_a.cfg', 'packtest_b.cfg', 'packtest_c.cfg', 'packtest_d.cfg', 'loadtest_a.cfg'

if ($Remove) {
	if (Test-Path $manifest) {
		foreach ($f in Get-Content $manifest) { $p = Join-Path $game $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p } }
		Remove-Item -LiteralPath $manifest
	}
	foreach ($s in $scripts) { $p = Join-Path $game $s; if (Test-Path $p) { Remove-Item -LiteralPath $p } }
	if (Test-Path $src) { Remove-Item -LiteralPath $src -Recurse }
	if ((Test-Path $tex) -and -not (Get-ChildItem -LiteralPath $tex -Recurse -File)) { Remove-Item -LiteralPath $tex -Recurse }
	"removed the test pack"
	return
}

if ($Run) {
	$cfg = if (Test-Path (Join-Path $game 'loadtest_a.cfg')) { 'loadtest_a.cfg' } else { 'packtest_a.cfg' }
	$runArgs = @{ Cfg = $cfg; Timeout = $Timeout; Width = $Width; Height = $Height; Data = $Data }
	if ($Release) { $runArgs.Release = $true }
	& (Join-Path $PSScriptRoot 'hl_run.ps1') @runArgs
	return
}

if (-not $Texconv -or -not (Test-Path -LiteralPath $Texconv)) { throw "-Texconv <texconv.exe> is needed to make the pack" }
if (($LoadSet.Count -or $Albedo -eq 'matched') -and -not $Export) { throw "-LoadSet and -Albedo matched need -Export <r_exporttextures' folder>" }
$ours = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }
if (Test-Path $tex) {
	$other = Get-ChildItem -LiteralPath $tex -Recurse -File | Where-Object { $ours -notcontains $_.FullName.Substring($game.Length + 1) }
	if ($other) { throw "data1\textures has other files (a texture pack?): $($other[0].FullName) ..." }
}
foreach ($f in $ours) { $p = Join-Path $game $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p } }	# a pack made again, its scripts too
if (Test-Path $src) { Remove-Item -LiteralPath $src -Recurse }
$tga = Join-Path $src 'tga'
$png = Join-Path $src 'png'
New-Item -ItemType Directory -Force $tga, (Join-Path $png 'textures'), $tex | Out-Null

if (-not ('TestPack' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class TestPack {
	// ---- tileable noise ----
	static int Mod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
	static uint Hash(int x, int y, int seed) {
		uint h = (uint)x * 374761393u + (uint)y * 668265263u + (uint)seed * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return h ^ (h >> 16);
	}
	public static double Rand(int x, int y, int seed) { return (Hash(x, y, seed) & 0xffffff) / 16777216.0; }
	static double Grad(int ix, int iy, double dx, double dy, int px, int py, int seed) {
		double a = Rand(Mod(ix, px), Mod(iy, py), seed) * 2.0 * Math.PI;
		return Math.Cos(a) * dx + Math.Sin(a) * dy;
	}
	static double Lerp(double a, double b, double t) { return a + (b - a) * t; }
	public static double Smooth(double e0, double e1, double x) {
		double t = Math.Max(0.0, Math.Min(1.0, (x - e0) / (e1 - e0)));
		return t * t * (3.0 - 2.0 * t);
	}
	// gradient noise with a period of px by py lattice cells, about -1..1
	public static double Noise(double x, double y, int px, int py, int seed) {
		int x0 = (int)Math.Floor(x), y0 = (int)Math.Floor(y);
		double fx = x - x0, fy = y - y0;
		double u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
		double a = Lerp(Grad(x0, y0, fx, fy, px, py, seed), Grad(x0 + 1, y0, fx - 1, fy, px, py, seed), u);
		double b = Lerp(Grad(x0, y0 + 1, fx, fy - 1, px, py, seed), Grad(x0 + 1, y0 + 1, fx - 1, fy - 1, px, py, seed), u);
		return Lerp(a, b, v) * 1.414;
	}
	// fBm over the texture (u, v in 0..1): cx by cy cells in its first octave
	public static double Fbm(double u, double v, int cx, int cy, int octaves, int seed) {
		double s = 0, a = 1, n = 0;
		for (int o = 0; o < octaves; o++) {
			s += a * Noise(u * cx, v * cy, cx, cy, seed + o * 1013);
			n += a; a *= 0.5; cx *= 2; cy *= 2;
		}
		return s / n;
	}

	// ---- a material: height (texels), linear albedo, roughness, metallic, linear emission ----
	public class Mat {
		public int W, H;
		public double[] Hgt, R, G, B, Rough, Metal, ER, EG, EB;
		public bool Emits;
		public Mat(int w, int h) {
			W = w; H = h; int n = w * h;
			Hgt = new double[n]; R = new double[n]; G = new double[n]; B = new double[n];
			Rough = new double[n]; Metal = new double[n]; ER = new double[n]; EG = new double[n]; EB = new double[n];
		}
	}

	// ashlar or brick courses: rows of n[row % n.Length] blocks, every other row shifted by half a block
	// (shift), mortar mw texels wide, bevels of bw texels, the color's linear albedo varied per block
	public static Mat Courses(int w, int h, int rows, int[] n, bool shift, double mw, double bw, double depth,
				  double[] color, double[] mortar, double rough, int seed) {
		var m = new Mat(w, h);
		int nc = Math.Max(2, w / 32), nr = Math.Max(2, h / 32);
		double rowH = h / (double)rows;
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double u = x / (double)w, v = y / (double)h;
			double fv = y / rowH; int row = (int)Math.Floor(fv); fv -= row;
			int nb = n[row % n.Length];
			double fu = u * nb + ((shift && (row & 1) == 1) ? 0.5 : 0.0);
			int col = (int)Math.Floor(fu); fu -= col; col = Mod(col, nb);
			double blockW = w / (double)nb;
			double dx = Math.Min(fu, 1 - fu) * blockW, dy = Math.Min(fv, 1 - fv) * rowH;
			// chipped edges: the distance to the edge varies along it
			double chip = (Fbm(u, v, nc * 2, nr * 2, 3, seed + 5) * 0.5 + 0.5) * bw * 1.2;
			double d = Math.Min(dx, dy) - mw * 0.5 - chip * 0.5;
			double face = Fbm(u, v, nc, nr, 5, seed + 11);
			double tone = 0.8 + 0.4 * Rand(col, row, seed + 3);
			double hueR = 1 + (Rand(col, row, seed + 4) - 0.5) * 0.12, hueB = 1 + (Rand(col, row, seed + 6) - 0.5) * 0.12;
			double speck = Fbm(u, v, nc * 8, nr * 8, 2, seed + 13);
			int i = y * w + x;
			if (d <= 0) {
				double mn = Fbm(u, v, nc * 4, nr * 4, 3, seed + 17);
				m.Hgt[i] = mn * 0.5;
				m.R[i] = mortar[0] * (0.85 + 0.3 * mn); m.G[i] = mortar[1] * (0.85 + 0.3 * mn); m.B[i] = mortar[2] * (0.85 + 0.3 * mn);
				m.Rough[i] = 0.97;
			} else {
				double bev = Smooth(0, bw, d);
				m.Hgt[i] = depth * bev + face * depth * 0.35 + speck * 0.4;
				double k = tone * (0.85 + 0.25 * face + 0.1 * speck) * (0.9 + 0.1 * bev);
				m.R[i] = color[0] * k * hueR; m.G[i] = color[1] * k; m.B[i] = color[2] * k * hueB;
				m.Rough[i] = Math.Min(1.0, rough + 0.08 * face - 0.04 * speck);
			}
		}
		return m;
	}

	// rounded stones (a tileable Voronoi of gx by gy jittered points): gaps g texels, rounded over r texels
	public static Mat Cobbles(int w, int h, int gx, int gy, double g, double r, double depth,
				  double[] color, double[] dirt, double rough, int seed) {
		var m = new Mat(w, h);
		double sx = w / (double)gx, sy = h / (double)gy;
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double px = x / sx, py = y / sy;
			int cx = (int)Math.Floor(px), cy = (int)Math.Floor(py);
			double f1 = 1e9, f2 = 1e9; int id1x = 0, id1y = 0;
			for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
				int ix = cx + i, iy = cy + j;
				double qx = ix + 0.2 + 0.6 * Rand(Mod(ix, gx), Mod(iy, gy), seed), qy = iy + 0.2 + 0.6 * Rand(Mod(ix, gx), Mod(iy, gy), seed + 1);
				double ddx = (px - qx) * sx, ddy = (py - qy) * sy, dd = Math.Sqrt(ddx * ddx + ddy * ddy);
				if (dd < f1) { f2 = f1; f1 = dd; id1x = Mod(ix, gx); id1y = Mod(iy, gy); } else if (dd < f2) f2 = dd;
			}
			double e = (f2 - f1) * 0.5 - g * 0.5;	// texels from the gap
			double u = x / (double)w, v = y / (double)h;
			double detail = Fbm(u, v, gx * 2, gy * 2, 5, seed + 9);
			double tone = 0.75 + 0.5 * Rand(id1x, id1y, seed + 2);
			int k = y * w + x;
			if (e <= 0) {
				m.Hgt[k] = detail * 0.5;
				m.R[k] = dirt[0] * (0.8 + 0.4 * detail); m.G[k] = dirt[1] * (0.8 + 0.4 * detail); m.B[k] = dirt[2] * (0.8 + 0.4 * detail);
				m.Rough[k] = 1.0;
			} else {
				double t = Math.Min(1.0, e / r), dome = Math.Sqrt(t * (2 - t));
				m.Hgt[k] = depth * dome + detail * depth * 0.25;
				double c = tone * (0.85 + 0.3 * detail);
				m.R[k] = color[0] * c; m.G[k] = color[1] * c; m.B[k] = color[2] * c;
				m.Rough[k] = Math.Min(1.0, rough - 0.12 * dome + 0.08 * detail);	// the tops worn smoother
			}
		}
		return m;
	}

	// an iron plate with rivets at the 3x3 grid's border points, a seam at the tile's edge, streaks, rust
	public static Mat Plate(int w, int h, double[] iron, double[] rust, int seed) {
		var m = new Mat(w, h);
		double rr = w / 22.0, seam = w / 90.0;
		double[] at = { 0.125, 0.5, 0.875 };
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double u = x / (double)w, v = y / (double)h;
			int i = y * w + x;
			double dent = Fbm(u, v, 3, 3, 3, seed);
			double streak = Fbm(u, v, 2, 24, 3, seed + 1);	// brushed along x
			double hgt = dent * 1.5;
			double ex = Math.Min(x, w - x), ey = Math.Min(y, h - y), edge = Math.Min(ex, ey);
			hgt -= 3.0 * (1 - Smooth(0, seam * 3, edge));	// the seam between plates
			double near = 1e9;
			for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
				if (a == 1 && b == 1) continue;
				double ddx = x - at[a] * w, ddy = y - at[b] * h, dd = Math.Sqrt(ddx * ddx + ddy * ddy);
				near = Math.Min(near, dd);
			}
			bool rivet = near < rr;
			if (rivet) hgt += 4.0 * Math.Sqrt(1 - (near / rr) * (near / rr));
			double rn = Fbm(u, v, 4, 4, 5, seed + 2) + 0.35 * (1 - Smooth(0, rr * 2.5, near)) + 0.3 * (1 - Smooth(0, seam * 8, edge));
			double rustAmt = Smooth(0.15, 0.45, rn);
			if (rivet) rustAmt *= 0.5;
			double rd = Fbm(u, v, 16, 16, 2, seed + 3);
			hgt += rustAmt * (0.8 + 0.6 * rd);
			m.Hgt[i] = hgt;
			double ik = 0.85 + 0.1 * streak;
			m.R[i] = Lerp(iron[0] * ik, rust[0] * (0.7 + 0.5 * rd), rustAmt);
			m.G[i] = Lerp(iron[1] * ik, rust[1] * (0.7 + 0.5 * rd), rustAmt);
			m.B[i] = Lerp(iron[2] * ik, rust[2] * (0.7 + 0.5 * rd), rustAmt);
			m.Metal[i] = 1 - Smooth(0.2, 0.6, rustAmt);
			m.Rough[i] = Lerp(rivet ? 0.3 : 0.38 + 0.12 * streak, 0.85, rustAmt);
		}
		return m;
	}

	// ripples: waves with whole wave numbers across the tile (it repeats), texels of height
	public static Mat Ripples(int w, int h, double amp, int seed) {
		var m = new Mat(w, h);
		int[,] k = { { 3, 1 }, { -2, 3 }, { 1, -4 }, { 5, 2 }, { -4, -3 }, { 7, -1 }, { 2, 6 } };
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double u = x / (double)w, v = y / (double)h, s = 0, n = 0;
			for (int j = 0; j < k.GetLength(0); j++) {
				double a = 1.0 / Math.Sqrt(k[j, 0] * k[j, 0] + k[j, 1] * k[j, 1]);
				s += a * Math.Sin(2 * Math.PI * (k[j, 0] * u + k[j, 1] * v) + 6.283 * Rand(j, 0, seed)); n += a;
			}
			int i = y * w + x;
			m.Hgt[i] = amp * (s / n + 0.3 * Fbm(u, v, 6, 6, 3, seed + 1));
			m.Rough[i] = 0.05;
		}
		return m;
	}

	// a stone panel with runes cut into it that glow: a square field in the middle (as the original's
	// carving), glyphs of strokes between points of a 3x3 lattice in a cols x rows grid
	public static Mat Runes(int w, int h, int cols, int rows, double[] stone, double[] glow, int seed) {
		var m = Courses(w, h, 1, new[] { 1 }, false, w / 60.0, w / 24.0, 4.0, stone, stone, 0.85, seed);
		double fx0 = w * 0.1, fx1 = w * 0.9, fy0 = h * 0.28, fy1 = h * 0.72, sw = w / 70.0;
		int ns = 0;
		var seg = new double[cols * rows * 5, 4];
		for (int gy = 0; gy < rows; gy++) for (int gx = 0; gx < cols; gx++) {
			double cx0 = fx0 + (fx1 - fx0) * (gx + 0.2) / cols, cx1 = fx0 + (fx1 - fx0) * (gx + 0.8) / cols;
			double cy0 = fy0 + (fy1 - fy0) * (gy + 0.15) / rows, cy1 = fy0 + (fy1 - fy0) * (gy + 0.85) / rows;
			int strokes = 3 + (int)(Rand(gx, gy, seed) * 3);
			for (int s = 0; s < strokes; s++) {
				int a = (int)(Rand(gx * 7 + s, gy, seed + 1) * 9), b = (int)(Rand(gx * 7 + s, gy, seed + 2) * 9);
				if (a == b) b = (a + 4) % 9;
				seg[ns, 0] = cx0 + (cx1 - cx0) * (a % 3) / 2.0; seg[ns, 1] = cy0 + (cy1 - cy0) * (a / 3) / 2.0;
				seg[ns, 2] = cx0 + (cx1 - cx0) * (b % 3) / 2.0; seg[ns, 3] = cy0 + (cy1 - cy0) * (b / 3) / 2.0;
				ns++;
			}
		}
		m.Emits = true;
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			int i = y * w + x;
			// the field: a shallow sunken square with a border groove
			double inx = Math.Min(x - fx0, fx1 - x), iny = Math.Min(y - fy0, fy1 - y), inside = Math.Min(inx, iny);
			if (inside > -sw) m.Hgt[i] -= 2.0 * Smooth(-sw, sw, inside);
			double dmin = 1e9;
			for (int s = 0; s < ns; s++) {
				double ax = seg[s, 0], ay = seg[s, 1], bx = seg[s, 2], by = seg[s, 3];
				double vx = bx - ax, vy = by - ay, t = ((x - ax) * vx + (y - ay) * vy) / (vx * vx + vy * vy);
				t = Math.Max(0, Math.Min(1, t));
				double ddx = x - ax - t * vx, ddy = y - ay - t * vy;
				dmin = Math.Min(dmin, Math.Sqrt(ddx * ddx + ddy * ddy));
			}
			double groove = 1 - Smooth(sw * 0.6, sw * 1.2, dmin);
			m.Hgt[i] -= 3.0 * groove;
			double e = 1 - Smooth(sw * 0.3, sw * 1.0, dmin);
			m.ER[i] = glow[0] * e; m.EG[i] = glow[1] * e; m.EB[i] = glow[2] * e;
			if (groove > 0) { m.R[i] *= 1 - 0.5 * groove; m.G[i] *= 1 - 0.5 * groove; m.B[i] *= 1 - 0.5 * groove; }
		}
		return m;
	}

	// glass for kind glass (its albedo is the tint of what is seen through it): panes in lead cames.
	// diamonds: quarries between lines u*a +- v*b = k (whole a, b: it repeats), colored from the palette;
	// else a grid of gx by gy panes. The lead is dark; a pane's tint varies a little (hand-made glass).
	public static Mat Leaded(int w, int h, bool diamonds, int a, int b, double lead, double[][] palette, int seed) {
		var m = new Mat(w, h);
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double u = x / (double)w, v = y / (double)h;
			double p, q; int ip, iq;
			if (diamonds) { p = u * a + v * b; q = u * a - v * b; }
			else { p = u * a; q = v * b; }
			ip = (int)Math.Floor(p); iq = (int)Math.Floor(q);
			double fp = p - ip, fq = q - iq;
			// the distance to the nearest line in texels: the lines' spacing is 1 / |grad p|
			double sp = diamonds ? 1 / Math.Sqrt((a / (double)w) * (a / (double)w) + (b / (double)h) * (b / (double)h)) : w / (double)a;
			double sq = diamonds ? sp : h / (double)b;
			double dp = Math.Min(fp, 1 - fp) * sp, dq = Math.Min(fq, 1 - fq) * sq, d = Math.Min(dp, dq);
			int i = y * w + x;
			// a pane's color by its center in the tile (so it repeats with the tile)
			int key;
			if (diamonds) {
				double cu = (ip + iq + 1) / (2.0 * a), cv = (ip - iq) / (2.0 * b);
				cu -= Math.Floor(cu); cv -= Math.Floor(cv);
				key = (int)Math.Round(cu * 997) * 1009 + (int)Math.Round(cv * 997);
			}
			else key = Mod(ip, a) * 1009 + Mod(iq, b);
			double[] c = palette[(int)(Rand(key, 3, seed) * palette.Length) % palette.Length];
			double wob = 0.95 + 0.05 * Fbm(u, v, 8, 8 * h / w, 3, seed + 1);
			double inLead = 1 - Smooth(lead * 0.5, lead * 0.5 + 1.5, d);
			m.R[i] = Lerp(c[0] * wob, 0.02, inLead); m.G[i] = Lerp(c[1] * wob, 0.02, inLead); m.B[i] = Lerp(c[2] * wob, 0.02, inLead);
			m.Hgt[i] = 1.5 * inLead;
			m.Rough[i] = 0.05;
		}
		return m;
	}

	// ---- the files ----
	static byte Enc(double linear) { return (byte)Math.Round(255.0 * Math.Pow(Math.Max(0, Math.Min(1, linear)), 1 / 2.2)); }
	static void Tga(string file, int w, int h, Func<int, byte[]> px) {
		var b = new byte[18 + w * h * 4];
		b[2] = 2; b[12] = (byte)w; b[13] = (byte)(w >> 8); b[14] = (byte)h; b[15] = (byte)(h >> 8); b[16] = 32; b[17] = 0x28;
		for (int i = 0; i < w * h; i++) {
			var c = px(i); int o = 18 + i * 4;
			b[o] = c[2]; b[o + 1] = c[1]; b[o + 2] = c[0]; b[o + 3] = 255;
		}
		File.WriteAllBytes(file, b);
	}
	// the albedo times k (linear), in the engine's 8-bit transfer (the 2.2 power)
	public static void Albedo(Mat m, string file, double k) {
		Tga(file, m.W, m.H, i => new[] { Enc(m.R[i] * k), Enc(m.G[i] * k), Enc(m.B[i] * k) });
	}
	public static void Emissive(Mat m, string file) {
		Tga(file, m.W, m.H, i => new[] { Enc(m.ER[i]), Enc(m.EG[i]), Enc(m.EB[i]) });
	}
	// glTF's occlusion (unused: 255), roughness, metallic
	public static void Orm(Mat m, string file) {
		Tga(file, m.W, m.H, i => new[] { (byte)255, (byte)Math.Round(255 * Math.Max(0, Math.Min(1, m.Rough[i]))),
						 (byte)Math.Round(255 * Math.Max(0, Math.Min(1, m.Metal[i]))) });
	}
	// the normal from the height (texels; blurred over a texel, the tile repeating), OpenGL's convention:
	// +Y towards the image's top, so a height rising down the image tilts the normal up
	public static void Normal(Mat m, string file, double strength) {
		int w = m.W, h = m.H;
		var s = new double[w * h];
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			double t = 0;
			for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) t += m.Hgt[Mod(y + j, h) * w + Mod(x + i, w)];
			s[y * w + x] = t / 9;
		}
		Tga(file, w, h, k => {
			int x = k % w, y = k / w;
			double dx = (s[y * w + Mod(x + 1, w)] - s[y * w + Mod(x - 1, w)]) * 0.5 * strength;
			double dy = (s[Mod(y + 1, h) * w + x] - s[Mod(y - 1, h) * w + x]) * 0.5 * strength;
			double nx = -dx, ny = dy, nz = 1, l = Math.Sqrt(nx * nx + ny * ny + nz * nz);
			return new[] { (byte)Math.Round(255 * (nx / l * 0.5 + 0.5)), (byte)Math.Round(255 * (ny / l * 0.5 + 0.5)), (byte)Math.Round(255 * (nz / l * 0.5 + 0.5)) };
		});
	}
	// the mean luminance of the albedo in linear light
	public static double MeanLuminance(Mat m) {
		double s = 0;
		for (int i = 0; i < m.W * m.H; i++) s += 0.2126 * m.R[i] + 0.7152 * m.G[i] + 0.0722 * m.B[i];
		return s / (m.W * m.H);
	}
	// the mean luminance in linear light (the 2.2 power) of 8-bit BGRA pixels
	public static double MeanLuminance8(byte[] bgra) {
		double s = 0; int n = bgra.Length / 4;
		for (int i = 0; i < n; i++)
			s += 0.2126 * Math.Pow(bgra[i * 4 + 2] / 255.0, 2.2) + 0.7152 * Math.Pow(bgra[i * 4 + 1] / 255.0, 2.2) + 0.0722 * Math.Pow(bgra[i * 4] / 255.0, 2.2);
		return s / n;
	}
}
'@
}

$written = New-Object System.Collections.Generic.List[string]
$rows = if ($Export) { Import-Csv (Join-Path $Export 'textures.csv') } else { @() }

# the original's mean luminance in linear light (the 2.2 power), from the export's PNG
Add-Type -AssemblyName PresentationCore, WindowsBase
function Original-Luminance([string]$file) {
	$s = [IO.File]::OpenRead((Join-Path $Export $file))	# a stream: a URI takes the # of #lava1 as a fragment
	try {
		$d = [System.Windows.Media.Imaging.BitmapDecoder]::Create($s, 'IgnoreColorProfile', 'OnLoad')
		$f = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap ($d.Frames[0], [System.Windows.Media.PixelFormats]::Bgra32, $null, 0)
		$px = New-Object byte[] ($f.PixelWidth * $f.PixelHeight * 4)
		$f.CopyPixels($px, $f.PixelWidth * 4, 0)
	} finally { $s.Close() }
	[TestPack]::MeanLuminance8($px)
}
# the export's row of a name in a map (the variant that map uses)
function Export-Row([string]$name, [string]$map) {
	$r = $rows | Where-Object { $_.name -eq $name -and (($_.'used in' -split ' ') -contains $map) } | Select-Object -First 1
	if (-not $r) { throw "the export has no $name used in $map" }
	$r
}

# a material's TGAs under their final names; the albedo times k (-Albedo matched: to the original's mean)
function Save([string]$name, $m, [string]$map = 'demo1', [switch]$Dielectric, [switch]$NoAlbedo, [double]$Normal = 1.0, [switch]$NoOrm) {
	$k = 1.0
	if ($Albedo -eq 'matched' -and $Dielectric -and -not $NoAlbedo) {
		$orig = Original-Luminance (Export-Row ($name -replace '#', '*') $map).file
		$k = $orig / [TestPack]::MeanLuminance($m)
		"  {0}: albedo x {1:N3} to the original's {2:N4}" -f $name, $k, $orig
	}
	$base = Join-Path $tga $name
	if (-not $NoAlbedo) { [TestPack]::Albedo($m, "$base.tga", $k) }
	if ($Normal -gt 0) { [TestPack]::Normal($m, "${base}_n.tga", $Normal) }
	if (-not $NoOrm) { [TestPack]::Orm($m, "${base}_orm.tga") }
	if ($m.Emits) { [TestPack]::Emissive($m, "${base}_e.tga") }
}
function Mat([string]$name, [string[]]$lines) {
	Set-Content -LiteralPath (Join-Path $tex "$name.mat") -Value $lines
	$written.Add("textures\$name.mat")
	Set-Content -LiteralPath $manifest -Value $written	# listed at once: a later failure leaves nothing unlisted
}

if ($LoadSet.Count) {
	# one stone material per world texture of the maps: courses of blocks at the texture's size
	$names = @{}
	foreach ($map in $LoadSet) {
		foreach ($r in $rows | Where-Object { $_.kind -in 'world', 'liquid' -and (($_.'used in' -split ' ') -contains $map) }) {
			$n = [IO.Path]::GetFileNameWithoutExtension($r.file)
			if (-not $names.ContainsKey($n)) { $names[$n] = $r }
		}
	}
	"load set: $($names.Count) textures of $($LoadSet -join ', ') at ${Scale}x, albedo $Albedo"
	foreach ($n in $names.Keys) {
		$r = $names[$n]; $w = [int]$r.width * $Scale; $h = [int]$r.height * $Scale
		$seed = 0; foreach ($ch in $n.ToCharArray()) { $seed = ($seed * 31 + [int]$ch) % 100003 }
		$m = [TestPack]::Courses($w, $h, [Math]::Max(2, [int]([int]$r.height / 16)), @([Math]::Max(1, [int]([int]$r.width / 32))), $true,
					 $Scale * 1.5, $Scale * 2.0, $Scale * 1.5, @(0.22, 0.20, 0.17), @(0.12, 0.11, 0.10), 0.85, $seed)
		$k = 1.0
		if ($Albedo -eq 'matched') { $k = (Original-Luminance $r.file) / [TestPack]::MeanLuminance($m) }
		$base = Join-Path $tga $n
		[TestPack]::Albedo($m, "$base.tga", $k)
		[TestPack]::Normal($m, "${base}_n.tga", 1.0)
		[TestPack]::Orm($m, "${base}_orm.tga")
	}
}
else {
	$S = $Scale
	"the test pack at ${S}x, albedo $Albedo"
	# stone (linear albedo: weathered sandstone about 0.2, granite cobbles 0.16, brick 0.2; mortar and dirt darker)
	Save 'rtex022' ([TestPack]::Courses(64 * $S, 176 * $S, 5, @(2, 1), $false, 2.5 * $S, 2.5 * $S, 2.0 * $S,
		@(0.25, 0.21, 0.16), @(0.13, 0.12, 0.10), 0.85, 22)) -Dielectric
	Save 'rtex005' ([TestPack]::Cobbles(64 * $S, 64 * $S, 5, 6, 1.5 * $S, 3.0 * $S, 2.5 * $S,
		@(0.17, 0.17, 0.165), @(0.07, 0.06, 0.05), 0.8, 5)) -Dielectric
	Save 'rtex429' ([TestPack]::Courses(64 * $S, 64 * $S, 6, @(3), $true, 1.5 * $S, 1.5 * $S, 1.5 * $S,
		@(0.28, 0.14, 0.08), @(0.2, 0.19, 0.17), 0.9, 429)) -Dielectric
	# metal: iron's reflectance (0.56, 0.57, 0.58) as the albedo, rust (0.25, 0.09, 0.03) a dielectric
	Save 'rtex011' ([TestPack]::Plate(64 * $S, 64 * $S, @(0.56, 0.57, 0.58), @(0.25, 0.09, 0.03), 11))
	Save 'rtex054' ([TestPack]::Plate(64 * $S, 64 * $S, @(0.56, 0.57, 0.58), @(0.25, 0.09, 0.03), 54))
	# water: ripples on the originals' colors; smooth, and water's 2 % head-on (specular 0.5 of Quake II RTX's 4 %)
	foreach ($n in '#rtex346', '#rtex078') {
		Save $n ([TestPack]::Ripples(64 * $S, 64 * $S, 0.6 * $S, 346)) -NoAlbedo -NoOrm
		Mat $n @('# water: smooth, 2 % head-on (IOR 1.33); the original colors', 'roughness 0.05', 'specular 0.5')
	}
	# emissive: runes cut into the stone panel, glowing cyan; the .mat's emissive scales _e (a color of 1 emits r_emissive_scale)
	Save 'rtex426' ([TestPack]::Runes(48 * $S, 80 * $S, 2, 2, @(0.21, 0.20, 0.19), @(0.3, 0.75, 1.0), 426)) -Dielectric
	Mat 'rtex426' @('# glowing runes: _e times 0.15 (r_emissive_scale 32: about 5 times GL''s fullbright)', 'emissive 0.15')
	# glass: the albedo is the tint of what is seen through it; lead cames dark
	$stained = [double[][]]@(@(0.85, 0.55, 0.15), @(0.7, 0.12, 0.1), @(0.15, 0.25, 0.75), @(0.25, 0.6, 0.25), @(0.85, 0.85, 0.72))
	$clear = [double[][]]@(@(0.86, 0.9, 0.87), @(0.82, 0.88, 0.85), @(0.88, 0.9, 0.9))
	Save 'rtex018' ([TestPack]::Leaded(64 * $S, 256 * $S, $true, 2, 8, 1.2 * $S, $stained, 18)) -Normal 0.5 -NoOrm
	Save 'rtex083' ([TestPack]::Leaded(64 * $S, 64 * $S, $false, 3, 3, 1.5 * $S, $clear, 83)) -Normal 0.5 -NoOrm
	Save 'rtex199' ([TestPack]::Leaded(32 * $S, 32 * $S, $false, 1, 1, 0.8 * $S, $clear, 199)) -Normal 0 -NoOrm
	foreach ($n in 'rtex018', 'rtex083', 'rtex199') { Mat $n @('# thin glass: refracts, reflects, tinted by the albedo', 'kind glass') }
}

# PNG as authored (texconv, 8-bit UNORM), then as they are or as DDS (pack_dds.ps1)
$tgas = @(Get-ChildItem -LiteralPath $tga -Filter *.tga -File)
for ($i = 0; $i -lt $tgas.Count; $i += 32) {
	$batch = @($tgas[$i..([Math]::Min($i + 31, $tgas.Count - 1))].FullName)	# one file stays an array
	$o = & $Texconv -nologo -y -o (Join-Path $png 'textures') -ft png -f R8G8B8A8_UNORM -m 1 @batch 2>&1
	if ($LASTEXITCODE -ne 0) { throw "texconv: $o" }
}
if ($Dds) {
	& (Join-Path $PSScriptRoot 'pack_dds.ps1') -Texconv $Texconv -Source (Join-Path $png 'textures') -Out $tex | Select-Object -Last 1
	foreach ($f in Get-ChildItem -LiteralPath $tex -File -Filter *.dds) { $written.Add("textures\$($f.Name)") }
}
else {
	foreach ($f in Get-ChildItem -LiteralPath (Join-Path $png 'textures') -File) {
		Copy-Item -LiteralPath $f.FullName -Destination $tex
		$written.Add("textures\$($f.Name)")
	}
}
Remove-Item -LiteralPath $tga -Recurse
Set-Content -LiteralPath $manifest -Value ($written | Sort-Object -Unique)	# and again with the scripts
$mb = (Get-ChildItem -LiteralPath $tex -File | Measure-Object -Property Length -Sum).Sum / 1MB
"{0} files in data1\textures ({1:N1} MB)" -f $written.Count, $mb

# ---- the scripts: one wait per frame, several on a line ----
function Waits([int]$n) { (@('wait') * $n) -join ';' }
function Shot([string]$name, [int]$view = 0) { "r_debugview $view"; Waits 40; "vk_screenshot $name 8"; Waits 12 }
function SetPos([string]$pos) { 'pause'; "vk_setpos $pos"; Waits 10; 'pause'; Waits 5 }
function View([string]$name, [string]$pos) { (SetPos $pos); "echo T56_VIEW $name"; 'vk_materials here' }
function Save-Script([string]$file, [object[]]$lines) {
	$text = (@($lines | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "$file is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	[IO.File]::WriteAllText((Join-Path $game $file), $text)
	$written.Add($file)
}
$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'flt_enable 1', 'viewsize 130', 'showpause 0', 'crosshair 0',
	'con_notifytime 0', 'r_materials 1', 'r_maplight_shape 2', 'color 0 0', 'playerclass 1')
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')
function Map([string]$map) { "map $map"; Waits 150; 'notarget'; 'noclip'; Waits 30; 'pause'; Waits 15 }

if ($LoadSet.Count) {
	# the albedo question: the lit image with and without bounces, frames averaged, the denoiser off
	$l = $start + @('flt_enable 0')
	foreach ($map in $LoadSet) {
		$l += (Map $map) + @("echo T56_LOAD $map", 'vk_materials', 'vk_world',
			'r_debugview 0', 'pt_num_bounce_rays 1', (Waits 20), "vk_screenshot load_${map}_lit 64", (Waits 70),
			'pt_num_bounce_rays 0', (Waits 20), "vk_screenshot load_${map}_direct 64", (Waits 70), 'pt_num_bounce_rays 1',
			'r_debugview 1', (Waits 10), "vk_screenshot load_${map}_base 1", (Waits 5), 'r_debugview 0', 'pause')
	}
	Save-Script 'loadtest_a.cfg' ($l + $end)
}
else {
	$a = $start + (Map 'demo1') + @('echo T56_START', 'vk_materials here', (Shot 'p_start'), (Shot 'p_start_base' 1),
		(Shot 'p_start_normal' 2), (Shot 'p_start_rm' 10), 'r_maplight_shape 0', (Shot 'p_start_phys'), 'r_maplight_shape 2',
		(View 'wall' '-1604 452 110 0 135'), (Shot 'p_wall'), (Shot 'p_wall_normal' 2), 'r_maplight_shape 0', (Shot 'p_wall_phys'), 'r_maplight_shape 2',
		(View 'cobbles' '-1781 -1321 -37 49 -163'), (Shot 'p_cobbles'), (Shot 'p_cobbles_rm' 10),
		'exec packtest_b.cfg')
	$b = @((View 'metal' '408 -1360 -214 0 90'), (Shot 'p_metal'), (Shot 'p_metal_rm' 10), (Shot 'p_metal_spec' 16),
		'r_maplight_shape 0', (Shot 'p_metal_phys'), 'r_maplight_shape 2',
		(View 'metal_side' '300 -1400 -214 0 45'), (Shot 'p_metal_side'), 'r_maplight_shape 0', (Shot 'p_metal_side_phys'), 'r_maplight_shape 2',
		(View 'pool' '-1300 2416 -480 20 180'), (Shot 'p_pool'), (Shot 'p_pool_normal' 2), 'r_maplight_shape 0', (Shot 'p_pool_phys'), 'r_maplight_shape 2',
		(View 'water' '-983 2300 -506 51 180'), (Shot 'p_water'),
		(View 'runes' '-608 -2120 374 0 0'), (Shot 'p_runes'), (Shot 'p_runes_emission' 13),
		'exec packtest_c.cfg')
	$c = @((View 'window_out' '-72 -1972 234 0 180'), (Shot 'p_window_out'), (Shot 'p_window_kinds' 3),
		(View 'window_in' '-280 -1972 234 0 0'), (Shot 'p_window_in'),
		(View 'skylight' '157 -1848 318 -51 180'), (Shot 'p_skylight'),
		'echo T56_LIST', 'vk_materials list', 'vk_materials problems', 'vk_world', 'pause',
		(Map 'cath'), (View 'cath_window' '-1546 1258 362 0 180'), (Shot 'p_cath_window'),
		(View 'cath_window_back' '-1482 500 373 0 -90'), (Shot 'p_cath_window_back'),
		(View 'cath_panes' '1061 1744 -211 -51 180'), (Shot 'p_cath_panes'), 'vk_world', 'pause', 'exec packtest_d.cfg')
	$d = @((Map 'village1'), (View 'village_glass' '511 2224 50 0 90'), (Shot 'p_village_glass'), (Shot 'p_village_kinds' 3),
		(View 'village_glass_back' '511 2345 50 0 -90'), (Shot 'p_village_glass_back'), 'vk_world', 'pause',
		# castle5's rtex199 is on 80 world faces: small tapered blocks standing in its lava; vk_world's glass line
		# counts the triangles connected past them (no view shows them well)
		'echo T56_CASTLE5', (Map 'castle5'), 'vk_world',
		'echo T56_PROBLEMS', 'vk_materials problems', 'pause') + $end
	Save-Script 'packtest_a.cfg' $a
	Save-Script 'packtest_b.cfg' $b
	Save-Script 'packtest_c.cfg' $c
	Save-Script 'packtest_d.cfg' $d
}
Set-Content -LiteralPath $manifest -Value ($written | Sort-Object -Unique)
