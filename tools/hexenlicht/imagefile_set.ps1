param([Parameter(Mandatory)][string]$Texconv, [string]$Data = '')

# Makes the image file test set of story 5.2 (docs/hexenlicht/TESTING.md,
# "Image files") in data1\textures\imgtest: generated images (not game data:
# a gradient, an alpha ramp, a 200x120 one, a normal map pattern, noise) as
# 32-bit TGAs in src\, and from them with Microsoft's texconv (DirectXTex;
# -Texconv, not in the repository) every format family the loader
# (vk_imagefile.c) reads (not each _SRGB twin of the uncompressed formats,
# nor both of BC5's FourCCs): PNG (8 and 16 bits, grey), TGA, DDS (BC7, BC7_SRGB,
# BC5 with the DX10 and the older header, RGBA8 and BGRA8, BGRX8, with and
# without mips); written here: DDS files whose X byte is 0 (the loader must
# make the alpha 255), DDS files whose mip levels are flat colors of their
# own (RGBA8, BC7 and BC5 blocks of one color: a quarter-size shot must show
# level 2's color, which proves the file's levels are the ones sampled), one
# with two of its levels (the rest blitted from the second: its color), KTX2
# files wrapping those DDS files' levels (stored smallest first as Khronos's
# tools do; no data format descriptor, which the loader doesn't read), the
# lookup order (order: .tga, .dds, .ktx2; order2: a .dds and a .ktx2 of
# other content) and files it must refuse (BC1, BC5_SNORM, a float format,
# cut short, supercompressed, not an image, empty, a wildcard; and two
# missing names). texconv's decodes of the lossy and converted files and the
# expected flat colors go to ref\. Writes the test scripts imgtest_a.cfg
# (hl_run.ps1 -Cfg imgtest_a.cfg runs it all), _b and _c into data1, and
# shots.txt (each screenshot's file, scale, mode, reference and check) for
# imagefile_compare.ps1. The set is generated: delete data1\textures\imgtest
# and the scripts afterwards. Add-Type keeps the C# helper for the pwsh
# session: after changing it, run the script in a new one.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$set = Join-Path $game 'textures\imgtest'
$src = Join-Path $set 'src'
$ref = Join-Path $set 'ref'
New-Item -ItemType Directory -Force $src, $ref | Out-Null

if (-not ('ImageFileSet' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class ImageFileSet {
	// RGBA rows, top first
	static byte[] Make(int w, int h, Func<int,int,int[]> px) {
		var o = new byte[w * h * 4];
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			var c = px(x, y); int p = (y * w + x) * 4;
			o[p] = (byte)c[0]; o[p + 1] = (byte)c[1]; o[p + 2] = (byte)c[2]; o[p + 3] = (byte)c[3];
		}
		return o;
	}
	public static void Tga(string file, int w, int h, byte[] rgba) {
		var b = new byte[18 + w * h * 4];
		b[2] = 2; b[12] = (byte)w; b[13] = (byte)(w >> 8); b[14] = (byte)h; b[15] = (byte)(h >> 8); b[16] = 32; b[17] = 0x28;
		for (int i = 0; i < w * h; i++) {
			b[18 + i * 4] = rgba[i * 4 + 2]; b[19 + i * 4] = rgba[i * 4 + 1]; b[20 + i * 4] = rgba[i * 4]; b[21 + i * 4] = rgba[i * 4 + 3];
		}
		File.WriteAllBytes(file, b);
	}
	static byte[] Grad() { return Make(256, 128, (x, y) => new[] { x, (y * 2) % 256, ((x / 16 + y / 16) % 2 == 1) ? 200 : 40, 255 }); }
	public static void Sources(string dir) {
		Tga(Path.Combine(dir, "grad.tga"), 256, 128, Grad());
		Tga(Path.Combine(dir, "alpha.tga"), 128, 128, Make(128, 128, (x, y) => new[] { 250, (60 + y) % 256, 30, (x * 2) % 256 }));
		Tga(Path.Combine(dir, "npot.tga"), 200, 120, Make(200, 120, (x, y) => new[] { x * 255 / 199, 255 - y * 2, (x * y) % 256, 255 }));
		Tga(Path.Combine(dir, "normal.tga"), 128, 128, Make(128, 128, (x, y) => {
			double nx = Math.Sin(x / 128.0 * 6.2832 * 2) * 0.5, ny = Math.Cos(y / 128.0 * 6.2832 * 2) * 0.5;
			double nz = Math.Sqrt(Math.Max(0, 1 - nx * nx - ny * ny));
			return new[] { (int)((nx * 0.5 + 0.5) * 255), (int)((ny * 0.5 + 0.5) * 255), (int)((nz * 0.5 + 0.5) * 255), 255 };
		}));
		var rnd = new Random(52);
		Tga(Path.Combine(dir, "noise.tga"), 256, 256, Make(256, 256, (x, y) => new[] { rnd.Next(256), rnd.Next(256), rnd.Next(256), 255 }));
	}
	// the flat colors of the mip levels (odd values: BC7 mode 6 keeps them exactly with its p-bit 1)
	public static readonly int[][] Levels = {
		new[] { 255, 1, 1 }, new[] { 1, 255, 1 }, new[] { 1, 1, 255 }, new[] { 255, 255, 1 }, new[] { 255, 1, 255 },
		new[] { 1, 255, 255 }, new[] { 129, 129, 129 }, new[] { 65, 65, 65 }, new[] { 255, 255, 255 } };
	public static void Flat(string file, int w, int h, int level) {
		var c = Levels[level]; Tga(file, w, h, Make(w, h, (x, y) => new[] { c[0], c[1], c[2], 255 }));
	}
	static void Header(BinaryWriter o, int w, int h, int levels, uint pfFlags, uint fourcc, uint bits, uint r, uint g, uint b, uint a, int dxgi) {
		o.Write(0x20534444u); o.Write(124u); o.Write(0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u); o.Write((uint)h); o.Write((uint)w);
		o.Write(0u); o.Write(0u); o.Write((uint)levels); for (int i = 0; i < 11; i++) o.Write(0u);
		o.Write(32u); o.Write(pfFlags); o.Write(fourcc); o.Write(bits); o.Write(r); o.Write(g); o.Write(b); o.Write(a);
		o.Write(0x1000u | (levels > 1 ? 0x400008u : 0u)); o.Write(0u); o.Write(0u); o.Write(0u); o.Write(0u);
		if (dxgi > 0) { o.Write((uint)dxgi); o.Write(3u); o.Write(0u); o.Write(1u); o.Write(0u); }
	}
	// one level of 32-bit texels from RGBA: kind bgrx10 (DXGI B8G8R8X8), xrgb9 (older header, BGRX masks), xbgr9 (RGBX masks); X = 0
	public static void DdsX(string file, string tga, string kind) {
		var t = File.ReadAllBytes(tga); int w = t[12] | (t[13] << 8), h = t[14] | (t[15] << 8);
		var o = new BinaryWriter(File.Create(file));
		if (kind == "bgrx10") Header(o, w, h, 1, 0x4, 0x30315844, 0, 0, 0, 0, 0, 88);
		else if (kind == "xrgb9") Header(o, w, h, 1, 0x40, 0, 32, 0xff0000, 0xff00, 0xff, 0, 0);
		else Header(o, w, h, 1, 0x40, 0, 32, 0xff, 0xff00, 0xff0000, 0, 0);
		for (int i = 0; i < w * h; i++) {
			byte b = t[18 + i * 4], g = t[19 + i * 4], r = t[20 + i * 4];
			if (kind == "xbgr9") { o.Write(r); o.Write(g); o.Write(b); } else { o.Write(b); o.Write(g); o.Write(r); }
			o.Write((byte)0);
		}
		o.Close();
	}
	// flat-colored mip levels 0..count-1 of a 256x128 image: kind rgba10 (DXGI R8G8B8A8), bc7 (mode 6 blocks), bc5
	public static void DdsLevels(string file, string kind, int count) {
		int w = 256, h = 128;
		var o = new BinaryWriter(File.Create(file));
		Header(o, w, h, count, 0x4, 0x30315844, 0, 0, 0, 0, 0, kind == "rgba10" ? 28 : kind == "bc7" ? 98 : 83);
		for (int l = 0; l < count; l++) {
			int lw = Math.Max(1, w >> l), lh = Math.Max(1, h >> l); var c = Levels[l];
			if (kind == "rgba10") { for (int i = 0; i < lw * lh; i++) { o.Write((byte)c[0]); o.Write((byte)c[1]); o.Write((byte)c[2]); o.Write((byte)255); } continue; }
			int blocks = ((lw + 3) / 4) * ((lh + 3) / 4);
			for (int i = 0; i < blocks; i++) o.Write(kind == "bc7" ? Bc7Flat(c) : Bc5Flat(c));
		}
		o.Close();
	}
	// BC7 mode 6: endpoints (7 bits + p-bit 1) both the color, every index 0
	static byte[] Bc7Flat(int[] c) {
		var bits = new System.Collections.BitArray(128); int at = 0;
		Action<int, int> put = (v, n) => { for (int i = 0; i < n; i++) bits[at++] = ((v >> i) & 1) != 0; };
		put(1 << 6, 7);
		for (int ch = 0; ch < 4; ch++) { int q = (ch < 3 ? c[ch] : 255) >> 1; put(q, 7); put(q, 7); }
		put(1, 1); put(1, 1);
		var b = new byte[16]; bits.CopyTo(b, 0); return b;
	}
	// BC5: two BC4 blocks, both endpoints the value, every index 0
	static byte[] Bc5Flat(int[] c) {
		var b = new byte[16]; b[0] = b[1] = (byte)c[0]; b[8] = b[9] = (byte)c[1]; return b;
	}
	// a DX10-header DDS's levels as a KTX2 (level index level 0 first, data smallest level first)
	public static void Ktx2(string dds, string ktx2, uint supercompression, int cut) {
		var b = File.ReadAllBytes(dds);
		uint h = BitConverter.ToUInt32(b, 12), w = BitConverter.ToUInt32(b, 16), levels = Math.Max(1u, BitConverter.ToUInt32(b, 28));
		uint dxgi = BitConverter.ToUInt32(b, 128), vkf;
		switch (dxgi) { case 98: vkf = 145; break; case 99: vkf = 146; break; case 83: vkf = 141; break;
			case 28: vkf = 37; break; case 87: vkf = 44; break; default: throw new Exception("DXGI " + dxgi); }
		bool bc = dxgi == 98 || dxgi == 99 || dxgi == 83;
		var size = new long[levels]; var at = new long[levels]; long p = 148;
		for (int i = 0; i < levels; i++) {
			long lw = Math.Max(1, w >> i), lh = Math.Max(1, h >> i);
			size[i] = bc ? ((lw + 3) / 4) * ((lh + 3) / 4) * 16 : lw * lh * 4; at[i] = p; p += size[i];
		}
		var ms = new MemoryStream(); var o = new BinaryWriter(ms);
		o.Write(new byte[] { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a });
		o.Write(vkf); o.Write(1u); o.Write(w); o.Write(h); o.Write(0u); o.Write(0u); o.Write(1u); o.Write(levels); o.Write(supercompression);
		o.Write(0u); o.Write(0u); o.Write(0u); o.Write(0u); o.Write(0ul); o.Write(0ul);
		var pos = new long[levels]; long d = 80 + 24 * levels;
		for (int i = (int)levels - 1; i >= 0; i--) { pos[i] = d; d += size[i]; }
		for (int i = 0; i < levels; i++) { o.Write((ulong)pos[i]); o.Write((ulong)size[i]); o.Write((ulong)size[i]); }
		for (int i = (int)levels - 1; i >= 0; i--) o.Write(b, (int)at[i], (int)size[i]);
		o.Flush(); var r = ms.ToArray();
		if (cut > 0) Array.Resize(ref r, r.Length - cut);
		File.WriteAllBytes(ktx2, r);
	}
}
'@
}
[ImageFileSet]::Sources($src)

function Convert-Image([string]$In, [string]$Out, [string[]]$Opt) {
	$o = & $Texconv -nologo -y -o $Out @Opt (Join-Path $src $In) 2>&1
	if ($LASTEXITCODE -ne 0) { throw "texconv $Opt ${In}: $o" }
}
# name suffix, source, texconv options
$variants = @(
	@('', 'grad.tga', '-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('_16', 'grad.tga', '-ft', 'png', '-f', 'R16G16B16A16_UNORM', '-m', '1'),
	@('_grey', 'grad.tga', '-ft', 'png', '-f', 'R8_UNORM', '-m', '1'),
	@('_tc', 'grad.tga', '-ft', 'tga', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('_bc7', 'grad.tga', '-f', 'BC7_UNORM', '-dx10'),
	@('_bc7s', 'grad.tga', '-f', 'BC7_UNORM_SRGB', '-dx10', '-srgb'),
	@('_bc7nomip', 'grad.tga', '-f', 'BC7_UNORM', '-dx10', '-m', '1'),
	@('_rgba9', 'grad.tga', '-f', 'R8G8B8A8_UNORM', '-dx9'),
	@('_bgra9', 'grad.tga', '-f', 'B8G8R8A8_UNORM', '-dx9'),
	@('_bgra10', 'grad.tga', '-f', 'B8G8R8A8_UNORM', '-dx10'),
	@('_rgbanomip', 'grad.tga', '-f', 'R8G8B8A8_UNORM', '-dx10', '-m', '1'),
	@('_bgrx', 'grad.tga', '-f', 'B8G8R8X8_UNORM', '-dx10'),
	@('_bc7', 'npot.tga', '-f', 'BC7_UNORM', '-dx10'),
	@('', 'npot.tga', '-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('', 'alpha.tga', '-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('_bc7', 'alpha.tga', '-f', 'BC7_UNORM', '-dx10'),
	@('_rgba10', 'alpha.tga', '-f', 'R8G8B8A8_UNORM', '-dx10', '-m', '1'),
	@('_bgra10', 'alpha.tga', '-f', 'B8G8R8A8_UNORM', '-dx10', '-m', '1'),
	@('', 'normal.tga', '-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('_bc5', 'normal.tga', '-f', 'BC5_UNORM', '-dx10'),
	@('_bc5dx9', 'normal.tga', '-f', 'BC5_UNORM', '-dx9'),
	@('', 'noise.tga', '-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1'),
	@('_bc1', 'grad.tga', '-f', 'BC1_UNORM'),
	@('_bc5s', 'normal.tga', '-f', 'BC5_SNORM', '-dx10'),
	@('_f16', 'grad.tga', '-f', 'R16G16B16A16_FLOAT', '-dx10'))
foreach ($v in $variants) {
	$opt = @($v[2..($v.Count - 1)])
	if ($v[0]) { $opt += @('-sx', $v[0]) }
	Convert-Image $v[1] $set $opt
}
function In-Set([string]$f) { Join-Path $set $f }
[ImageFileSet]::DdsX((In-Set 'gradx_bgrx10.dds'), (Join-Path $src 'grad.tga'), 'bgrx10')
[ImageFileSet]::DdsX((In-Set 'gradx_xrgb9.dds'), (Join-Path $src 'grad.tga'), 'xrgb9')
[ImageFileSet]::DdsX((In-Set 'gradx_xbgr9.dds'), (Join-Path $src 'grad.tga'), 'xbgr9')
[ImageFileSet]::DdsLevels((In-Set 'levels.dds'), 'rgba10', 9)
[ImageFileSet]::DdsLevels((In-Set 'levels_bc7.dds'), 'bc7', 9)
[ImageFileSet]::DdsLevels((In-Set 'levels_bc5.dds'), 'bc5', 9)
[ImageFileSet]::DdsLevels((In-Set 'levels2.dds'), 'rgba10', 2)
foreach ($k in @(@('grad_bc7.dds', 'gradk_bc7.ktx2'), @('grad_bc7s.dds', 'gradk_bc7s.ktx2'), @('normal_bc5.dds', 'normalk_bc5.ktx2'),
		 @('grad_rgbanomip.dds', 'gradk_rgba.ktx2'), @('grad_bgra10.dds', 'gradk_bgra.ktx2'), @('npot_bc7.dds', 'npotk_bc7.ktx2'),
		 @('alpha_bgra10.dds', 'alphak_bgra.ktx2'), @('levels.dds', 'levelsk.ktx2'), @('levels_bc7.dds', 'levelsk_bc7.ktx2'))) {
	[ImageFileSet]::Ktx2((In-Set $k[0]), (In-Set $k[1]), 0, 0)
}
[ImageFileSet]::Ktx2((In-Set 'grad_bc7.dds'), (In-Set 'gradk_zstd.ktx2'), 2, 0)
[ImageFileSet]::Ktx2((In-Set 'grad_bc7.dds'), (In-Set 'gradk_cut.ktx2'), 0, 1000)
$b = [IO.File]::ReadAllBytes((In-Set 'grad_bc7.dds'))
[IO.File]::WriteAllBytes((In-Set 'grad_cut.dds'), $b[0..20000])
[IO.File]::WriteAllText((In-Set 'notdds.dds'), 'not a DDS file')
[IO.File]::WriteAllText((In-Set 'notpng.png'), 'not a PNG file')
[IO.File]::WriteAllBytes((In-Set 'empty.png'), [byte[]]@())
Copy-Item (In-Set 'grad_tc.tga') (In-Set 'order.tga')
Copy-Item (In-Set 'grad_bc7.dds') (In-Set 'order.dds')
Copy-Item (In-Set 'gradk_bc7.ktx2') (In-Set 'order.ktx2')
Copy-Item (In-Set 'grad_bc7.dds') (In-Set 'order2.dds')
Copy-Item (In-Set 'npotk_bc7.ktx2') (In-Set 'order2.ktx2')
# texconv's own decodes of the lossy and converted files, the bytes as they
# are (an _SRGB file into an _SRGB format: into UNORM texconv converts the curve)
foreach ($f in 'grad_grey.png', 'grad_bc7.dds', 'grad_bc7s.dds', 'npot_bc7.dds', 'alpha_bc7.dds', 'normal_bc5.dds') {
	$fmt = if ($f -match 'bc7s') { 'R8G8B8A8_UNORM_SRGB' } else { 'R8G8B8A8_UNORM' }
	$o = & $Texconv -nologo -y -o $ref -ft tga -f $fmt -m 1 (In-Set $f) 2>&1
	if ($LASTEXITCODE -ne 0) { throw "texconv decode ${f}: $o" }
}
[ImageFileSet]::Flat((Join-Path $ref 'level1.tga'), 64, 32, 1)
[ImageFileSet]::Flat((Join-Path $ref 'level2.tga'), 64, 32, 2)

# the screenshots: file, scale, mode (rgb; grey; alpha: over black; rg:
# BC5), reference (in the set), check (exact: equal; decode: within 4 of
# texconv's decode, mean 0.1; flat: within 2; box: the reference averaged
# over 4x4 texels, within 2)
$shots = @(
	'grad.png 1 rgb src/grad.tga exact', 'grad_16.png 1 rgb src/grad.tga exact', 'grad_grey.png 1 grey ref/grad_grey.tga exact',
	'grad_tc.tga 1 rgb src/grad.tga exact', 'order 1 rgb src/grad.tga exact',
	'grad_bc7.dds 1 rgb ref/grad_bc7.tga decode', 'grad_bc7s.dds 1 rgb ref/grad_bc7s.tga decode', 'grad_bc7nomip.dds 1 rgb ref/grad_bc7.tga decode',
	'grad_rgba9.dds 1 rgb src/grad.tga exact', 'grad_bgra9.dds 1 rgb src/grad.tga exact', 'grad_bgra10.dds 1 rgb src/grad.tga exact',
	'grad_rgbanomip.dds 1 rgb src/grad.tga exact', 'grad_bgrx.dds 1 rgb src/grad.tga exact',
	'gradx_bgrx10.dds 1 rgb src/grad.tga exact', 'gradx_xrgb9.dds 1 rgb src/grad.tga exact', 'gradx_xbgr9.dds 1 rgb src/grad.tga exact',
	'npot_bc7.dds 1 rgb ref/npot_bc7.tga decode', 'npot.png 1 rgb src/npot.tga exact',
	'alpha.png 1 alpha src/alpha.tga exact', 'alpha_bc7.dds 1 alpha ref/alpha_bc7.tga decode',
	'alpha_rgba10.dds 1 alpha src/alpha.tga exact', 'alphak_bgra.ktx2 1 alpha src/alpha.tga exact',
	'normal.png 1 rgb src/normal.tga exact', 'normal_bc5.dds 1 rg ref/normal_bc5.tga decode', 'normal_bc5dx9.dds 1 rg ref/normal_bc5.tga decode',
	'gradk_bc7.ktx2 1 rgb ref/grad_bc7.tga decode', 'gradk_bc7s.ktx2 1 rgb ref/grad_bc7s.tga decode', 'normalk_bc5.ktx2 1 rg ref/normal_bc5.tga decode',
	'gradk_rgba.ktx2 1 rgb src/grad.tga exact', 'gradk_bgra.ktx2 1 rgb src/grad.tga exact', 'npotk_bc7.ktx2 1 rgb ref/npot_bc7.tga decode',
	'order2 1 rgb ref/grad_bc7.tga decode',
	'levels.dds 0.25 rgb ref/level2.tga flat', 'levels_bc7.dds 0.25 rgb ref/level2.tga flat', 'levels_bc5.dds 0.25 rg ref/level2.tga flat',
	'levelsk.ktx2 0.25 rgb ref/level2.tga flat', 'levelsk_bc7.ktx2 0.25 rgb ref/level2.tga flat', 'levels2.dds 0.25 rgb ref/level1.tga flat',
	'noise.png 0.25 rgb src/noise.tga box')
$refuse = 'grad_bc1.dds normal_bc5s.dds grad_f16.dds grad_cut.dds gradk_zstd.ktx2 gradk_cut.ktx2 notdds.dds notpng.png empty.png grad* missing.png nothere' -split ' '
function Waits([int]$n) { @('wait') * $n }
$a = @(Waits 3) + @('vid_vsync 0', 'host_framerate 0.02', 'map demo1') + @(Waits 150) +
     @('vid_uiscale 1', 'gamma 1', 'viewsize 130', 'crosshair 0', 'r_drawviewmodel 0', 'con_notifytime 0', 'notarget') + @(Waits 20) +
     @('vk_info', 'exec imgtest_b.cfg')
$scripts = @(@(), @())
$order = @()
for ($n = 0; $n -lt $shots.Count; $n++) {
	$f, $scale = ($shots[$n] -split ' ')[0, 1]
	$scripts[[int]($n -ge 20)] += @("echo ==== SHOT $n $f $scale", "vk_imagefile textures/imgtest/$f $scale") + @(Waits 4) + @('screenshot') + @(Waits 2)
	$order += "$n $($shots[$n])"
}
$bl = $scripts[0] + @('exec imgtest_c.cfg')
$c = $scripts[1] + @('echo ==== CASE', 'vk_imagefile TEXTURES/IMGTEST/GRAD.PNG 1') + @(Waits 2) + @('echo ==== REFUSALS')
foreach ($f in $refuse) { $c += "vk_imagefile textures/imgtest/$f" }
$c += @('vk_imagefile', 'echo ==== END', 'vid_vsync 1', 'toggleconsole') + @(Waits 5) + @('quit')
Set-Content (Join-Path $game 'imgtest_a.cfg') $a
Set-Content (Join-Path $game 'imgtest_b.cfg') $bl
Set-Content (Join-Path $game 'imgtest_c.cfg') $c
Set-Content (Join-Path $set 'shots.txt') $order
"{0} files in {1}; run hl_run.ps1 -Cfg imgtest_a.cfg (data1\shots empty first: {2} screenshots), then imagefile_compare.ps1" -f (Get-ChildItem $set -File).Count, $set, $shots.Count
