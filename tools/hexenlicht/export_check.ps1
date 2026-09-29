param([Parameter(Mandatory)][string]$Texconv, [string]$Export = '', [string]$Data = '', [string[]]$Paks = @(), [switch]$Keep)

# Checks a texture export (r_exporttextures, story 5.4; docs/hexenlicht/TESTING.md,
# "Texture export") against the game's paks without the engine: reads the paks'
# maps (mip 0 of each world texture), models (the skins of both MDL formats,
# the first one flood-filled as gl_model.c does before its CRC), sprites (the
# frames) and gfx/skin<n>.lmp itself, converts the 8-bit pixels by the rules
# of GL_LoadTexture's conversion (MATERIALS.md's colors: the palette, index
# 255 transparent with a neighbour's color, the holey, transparent and
# special-trans modes), names them as MATERIALS.md does (~<crc> on every
# variant of a name with more than one set of pixels), and compares:
# textures.csv row by row (every file a row, the CRC, size, kind, alpha,
# variants, maps), and every PNG decoded by Microsoft's texconv (DirectXTex,
# -Texconv, not in the repository; another PNG reader than the engine's
# stb_image), the colors exactly, the alpha where the file has one. Prints
# the counts and each difference (the first 30): also a row's engine name
# (a name's first occurrence in the search path), a second row of a file,
# PNGs the manifest doesn't list. The export is -Export (the default:
# portals\export, made with -portals), the paks -Paks in the order the
# engine adds them (data1's pak0, pak1, portals' pak3; read the other way
# round, the search path's order). The decoded files go into
# <export>\check, deleted afterwards (-Keep keeps them).
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
if (-not $Export) { $Export = Join-Path $Data 'portals\export' }
if (-not $Paks) { $Paks = (Join-Path $Data 'data1\pak0.pak'), (Join-Path $Data 'data1\pak1.pak'), (Join-Path $Data 'portals\pak3.pak') }
# .NET resolves relative paths against the process's folder, not $PWD
$Export = Convert-Path -LiteralPath $Export
$Paks = @($Paks | ForEach-Object { Convert-Path -LiteralPath $_ })
$csvPath = Join-Path $Export 'textures.csv'
if (-not (Test-Path $csvPath)) { throw "no $csvPath" }

if (-not ('ExportCheck' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class ExpTex {
	public string Name, Identifier, Kind, Used, From;
	public int W, H, Mode;		// Mode: 1 alpha, 2 transparent, 4 holey, 8 special-trans
	public ushort Crc;
	public byte[] Px;
}

public static class ExportCheck {
	static readonly ushort[] table = MakeTable();
	static ushort[] MakeTable() {
		var t = new ushort[256];
		for (int i = 0; i < 256; i++) {
			int c = i << 8;
			for (int k = 0; k < 8; k++) c = ((c & 0x8000) != 0) ? (c << 1) ^ 0x1021 : c << 1;
			t[i] = (ushort)c;
		}
		return t;
	}
	public static ushort Crc(byte[] b) {
		ushort crc = 0xffff;
		foreach (byte x in b) crc = (ushort)((crc << 8) ^ table[(crc >> 8) ^ x]);
		return crc;
	}
	static int I(byte[] b, int o) { return BitConverter.ToInt32(b, o); }
	static string Spec(string id) {
		string n = id.ToLowerInvariant().Replace('*', '#');
		return n.EndsWith(".lmp") ? n.Substring(0, n.Length - 4) : n;
	}

	public static uint[] Pal = new uint[256];	// R,G,B,A in memory as the engine's d_8to24table
	public static void SetPalette(byte[] p) {
		for (int i = 0; i < 256; i++) Pal[i] = (uint)(p[i * 3] | (p[i * 3 + 1] << 8) | (p[i * 3 + 2] << 16)) | 0xff000000u;
		Pal[255] &= 0x00ffffffu;
	}

	public static List<ExpTex> Textures = new List<ExpTex>();
	static void Add(string id, string kind, byte[] data, int ofs, int w, int h, int mode, string used, string from) {
		var t = new ExpTex { Identifier = id, Name = Spec(id), Kind = kind, W = w, H = h, Mode = mode, Used = used, From = from };
		t.Px = new byte[w * h];
		Buffer.BlockCopy(data, ofs, t.Px, 0, w * h);
		t.Crc = Crc(t.Px);
		Textures.Add(t);
	}

	// gl_model.c's flood fill of a skin's background from the top left
	static void Fill(byte[] s, int o, int w, int h) {
		int fill = s[o], filled = 0;
		for (int i = 0; i < 256; i++) if (Pal[i] == 255u) { filled = i; break; }
		if (fill == filled || fill == 255) return;
		var fx = new short[0x1000]; var fy = new short[0x1000];
		int inp = 1, outp = 0; fx[0] = 0; fy[0] = 0;
		while (outp != inp) {
			int x = fx[outp], y = fy[outp], fdc = filled, pos = o + x + w * y;
			outp = (outp + 1) & 0xfff;
			int[] offs = { -1, 1, -w, w }, dx = { -1, 1, 0, 0 }, dy = { 0, 0, -1, 1 };
			bool[] ok = { x > 0, x < w - 1, y > 0, y < h - 1 };
			for (int k = 0; k < 4; k++) {
				if (!ok[k]) continue;
				int p = pos + offs[k];
				if (s[p] == fill) { s[p] = 255; fx[inp] = (short)(x + dx[k]); fy[inp] = (short)(y + dy[k]); inp = (inp + 1) & 0xfff; }
				else if (s[p] != 255) fdc = s[p];
			}
			s[pos] = (byte)fdc;
		}
	}

	public static void ReadPak(string path, string label) {
		byte[] b = File.ReadAllBytes(path);
		int dirofs = I(b, 4), dirlen = I(b, 8);
		for (int e = 0; e < dirlen / 64; e++) {
			int o = dirofs + e * 64;
			string name = Encoding.ASCII.GetString(b, o, 56).Split('\0')[0];
			int pos = I(b, o + 56), len = I(b, o + 60);
			string low = name.ToLowerInvariant();
			if (low.StartsWith("maps/") && low.EndsWith(".bsp")) Map(b, pos, len, low.Substring(5, low.Length - 9), label);
			else if ((low.StartsWith("models/") || low.StartsWith("gfx/")) && (low.EndsWith(".mdl") || low.EndsWith(".spr"))) {
				byte[] f = new byte[len]; Buffer.BlockCopy(b, pos, f, 0, len);
				if (low.EndsWith(".mdl")) Model(f, name, label); else Sprite(f, name, label);
			}
			else if (low.StartsWith("gfx/skin") && low.EndsWith(".lmp"))
				Add(name, "picture", b, pos + 8, I(b, pos), I(b, pos + 4), 1, low, label);
		}
	}
	static void Map(byte[] b, int pos, int len, string map, string label) {
		if (I(b, pos + 4 + 2 * 8 + 4) == 0) return;	// no textures
		int lump = pos + I(b, pos + 4 + 2 * 8), n = I(b, lump);
		for (int i = 0; i < n; i++) {
			int mo = I(b, lump + 4 + 4 * i);
			if (mo < 0) continue;
			int m = lump + mo;
			string id = Encoding.ASCII.GetString(b, m, 16).Split('\0')[0];
			string kind = id.StartsWith("sky") ? "sky" : id.StartsWith("*") ? "liquid" : "world";
			Add(id, kind, b, m + 40, I(b, m + 16), I(b, m + 20), 0, map, label);
		}
	}
	static void Model(byte[] f, string name, string label) {
		bool newfmt = I(f, 0) == 0x4f504152;	// RAPO
		int numskins = I(f, 48), w = I(f, 52), h = I(f, 56), flags = I(f, 76), s = w * h;
		int mode = ((flags & 0x1000) != 0) ? 2 : ((flags & 0x4000) != 0) ? 4 : ((flags & 0x8000) != 0) ? 8 : 0;
		int p = newfmt ? 88 : 84, skin = p + 4;
		for (int i = 0; i < numskins; i++) {
			if (I(f, p) == 0) {
				Fill(f, skin, w, h);
				Add(name + "_" + i, "skin", f, p + 4, w, h, mode, name.ToLowerInvariant(), label);
				p += 4 + s;
			} else {
				int count = I(f, p + 4);
				p += 8 + 4 * count;
				for (int j = 0; j < count; j++) {
					Fill(f, skin, w, h);
					Add(name + "_" + i + "_" + j, "skin", f, p, w, h, mode, name.ToLowerInvariant(), label);
					p += s;
				}
			}
		}
	}
	static void Sprite(byte[] f, string name, string label) {
		int n = I(f, 24), p = 36;
		for (int i = 0; i < n; i++) {
			int type = I(f, p); p += 4;
			int count = 1, baseNum = i;
			if (type != 0) { count = I(f, p); p += 4 + 4 * count; baseNum = i * 100; }
			for (int j = 0; j < count; j++) {
				int w = I(f, p + 8), h = I(f, p + 12);
				Add(name + "_" + (baseNum + j), "sprite", f, p + 16, w, h, 1, name.ToLowerInvariant(), label);
				p += 16 + w * h;
			}
		}
	}

	// the colors GL_LoadTexture uploads (R,G,B,A in memory); hasAlpha: its TEX_ALPHA after the conversion
	static readonly int[] ColorIndex = { 0, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 199, 207, 223, 231 };
	static readonly int[] ColorPercent = { 25, 51, 76, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 237, 247 };
	public static uint[] Convert(ExpTex t, out bool hasAlpha) {
		int s = t.W * t.H, w = t.W;
		var c = new uint[s];
		byte[] d = t.Px;
		hasAlpha = false;
		if (t.Mode == 0) { for (int i = 0; i < s; i++) c[i] = Pal[d[i]]; return c; }
		for (int i = 0; i < s; i++) {
			int p = d[i];
			c[i] = Pal[p];
			if (p == 255) {
				hasAlpha = true;
				int q = 0;
				if (i > w && d[i - w] != 255) q = d[i - w];
				else if (i < s - w && d[i + w] != 255) q = d[i + w];
				else if (i > 0 && d[i - 1] != 255) q = d[i - 1];
				else if (i < s - 1 && d[i + 1] != 255) q = d[i + 1];
				c[i] = (Pal[q] & 0xffffffu) | (c[i] & 0xff000000u);
			}
			if ((t.Mode & 2) != 0) {
				if (p == 0) c[i] &= 0xffffffu;
				else if ((p & 1) != 0) c[i] = (c[i] & 0xffffffu) | (84u << 24);	/* 255 * 0.33 */
				else c[i] |= 0xff000000u;
			} else if ((t.Mode & 4) != 0) {
				if (p == 0) c[i] &= 0xffffffu;
			} else if ((t.Mode & 8) != 0) {
				c[i] = (Pal[ColorIndex[p >> 4]] & 0xffffffu) | ((uint)(255 - ColorPercent[p & 15]) << 24);
			}
		}
		if ((t.Mode & 14) != 0) hasAlpha = true;
		return c;
	}

	// a 24- or 32-bit uncompressed TGA as R,G,B,A per pixel, top row first
	public static uint[] ReadTga(string path, out int w, out int h) {
		byte[] b = File.ReadAllBytes(path);
		if (b[2] != 2) throw new Exception(path + ": not an uncompressed true color TGA");
		w = b[12] | (b[13] << 8); h = b[14] | (b[15] << 8);
		int bpp = b[16] / 8, o = 18 + b[0];
		bool top = (b[17] & 0x20) != 0;
		var c = new uint[w * h];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int i = o + ((top ? y : h - 1 - y) * w + x) * bpp;
				uint a = (bpp == 4) ? b[i + 3] : 255u;
				c[y * w + x] = (uint)(b[i + 2] | (b[i + 1] << 8) | (b[i] << 16)) | (a << 24);
			}
		return c;
	}
}
'@
}

[ExportCheck]::Textures.Clear()
$diffs = New-Object System.Collections.Generic.List[string]
function AddDiff([string]$s) { $diffs.Add($s) }	# not Diff: PowerShell's alias of Compare-Object wins over a function
$pal = $null
# the search path's order (a later pak first), in which the export keeps a name's first occurrence
[array]::Reverse($Paks)
foreach ($pak in $Paks) {
	# the palette: gfx/palette.lmp of the first pak that has one
	if (-not $pal) {
		$b = [IO.File]::ReadAllBytes($pak); $dirofs = [BitConverter]::ToInt32($b, 4); $dirlen = [BitConverter]::ToInt32($b, 8)
		for ($e = 0; $e -lt $dirlen / 64; $e++) {
			$o = $dirofs + $e * 64
			if ([Text.Encoding]::ASCII.GetString($b, $o, 56).Split([char]0)[0] -eq 'gfx/palette.lmp') {
				$pal = New-Object byte[] 768; [Array]::Copy($b, [BitConverter]::ToInt32($b, $o + 56), $pal, 0, 768)
			}
		}
		if ($pal) { [ExportCheck]::SetPalette($pal) }
	}
	$label = (Split-Path (Split-Path $pak) -Leaf) + '/' + (Split-Path $pak -Leaf)
	[ExportCheck]::ReadPak($pak, $label)
}
if (-not $pal) { throw 'no gfx/palette.lmp in the paks' }

# the variants: one per name and CRC, merged over the occurrences
$byName = @{}
foreach ($t in [ExportCheck]::Textures) {
	if (-not $byName[$t.Name]) { $byName[$t.Name] = [ordered]@{} }
	$v = $byName[$t.Name]["$($t.Crc)"]
	if (-not $v) { $byName[$t.Name]["$($t.Crc)"] = $v = @{ T = $t; Used = @{}; From = @{} } }
	elseif ($v.T.W -ne $t.W -or $v.T.H -ne $t.H -or [Convert]::ToBase64String($v.T.Px) -ne [Convert]::ToBase64String($t.Px)) { AddDiff "COLLISION: $($t.Name) $('{0:x4}' -f $t.Crc) (two images, one CRC)" }
	$v.Used[$t.Used] = 1; $v.From[$t.From] = 1
}
$expected = @{}
foreach ($name in $byName.Keys) {
	foreach ($v in $byName[$name].Values) {
		$file = if ($byName[$name].Count -gt 1) { 'textures/{0}~{1:x4}.png' -f $name, $v.T.Crc } else { "textures/$name.png" }
		$expected[$file] = @{ V = $v; Variants = $byName[$name].Count }
	}
}

# texconv decodes every PNG (the directory structure kept)
$check = Join-Path $Export 'check'
if (Test-Path $check) { throw "$check exists: delete it first" }
New-Item -ItemType Directory $check | Out-Null	# texconv -r:keep makes the folders in it, not it
$out = & $Texconv -nologo -y -r:keep -o $check -ft tga -f R8G8B8A8_UNORM -m 1 (Join-Path $Export 'textures\*.png') 2>&1
if ($LASTEXITCODE -ne 0) { throw "texconv: $($out | Select-Object -Last 3)" }

$csv = Import-Csv $csvPath
$seen = @{}; $pixels = 0; $alphaFiles = 0
foreach ($r in $csv) {
	$e = $expected[$r.file]
	if (-not $e) { AddDiff "$($r.file): not expected"; continue }
	if ($seen[$r.file]) { AddDiff "$($r.file): a second row"; continue }
	$seen[$r.file] = 1
	$t = $e.V.T
	if ($r.crc -ne ('{0:x4}' -f $t.Crc) -or [int]$r.width -ne $t.W -or [int]$r.height -ne $t.H -or $r.kind -ne $t.Kind -or [int]$r.variants -ne $e.Variants) {
		AddDiff "$($r.file): the row says $($r.crc) $($r.width)x$($r.height) $($r.kind) $($r.variants), expected $('{0:x4}' -f $t.Crc) $($t.W)x$($t.H) $($t.Kind) $($e.Variants)"
	}
	if ($r.name -cne $t.Identifier) { AddDiff "$($r.file): the name $($r.name), expected $($t.Identifier)" }
	$used = ($e.V.Used.Keys | Sort-Object) -join ' '; $rowUsed = ($r.'used in' -split ' ' | Sort-Object) -join ' '
	$from = ($e.V.From.Keys | Sort-Object) -join ' '; $rowFrom = ($r.from -split ' ' | Sort-Object) -join ' '
	if ($used -ne $rowUsed -or $from -ne $rowFrom) { AddDiff "$($r.file): used in '$rowUsed' from '$rowFrom', expected '$used' from '$from'" }

	$hasAlpha = $false
	$c = [ExportCheck]::Convert($t, [ref]$hasAlpha)
	$kind = 'none'
	if ($hasAlpha) { foreach ($x in $c) { $a = $x -shr 24; if ($a -ne 255) { $kind = 'coverage'; if ($a -ne 0) { $kind = 'translucent'; break } } } }
	if ($r.alpha -ne $kind) { AddDiff "$($r.file): alpha $($r.alpha), expected $kind" }
	$tga = Join-Path $check ($r.file -replace '^textures/', '' -replace '\.png$', '.tga')
	if (-not (Test-Path -LiteralPath $tga)) { AddDiff "$($r.file): texconv made no $tga"; continue }
	$w = 0; $h = 0
	$d = [ExportCheck]::ReadTga($tga, [ref]$w, [ref]$h)
	if ($w -ne $t.W -or $h -ne $t.H) { AddDiff "$($r.file): decoded ${w}x$h"; continue }
	$mask = if ($kind -eq 'none') { 0xffffff } else { 0xffffffff }
	$bad = 0; $first = -1
	for ($i = 0; $i -lt $c.Length; $i++) {
		if ((($c[$i] -bxor $d[$i]) -band $mask) -ne 0) { $bad++; if ($first -lt 0) { $first = $i } }
		elseif ($kind -eq 'none' -and ($d[$i] -shr 24) -ne 255) { $bad++; if ($first -lt 0) { $first = $i } }
	}
	if ($bad) { AddDiff ("{0}: {1} texels differ, the first at {2},{3}: {4:x8}, expected {5:x8}" -f $r.file, $bad, ($first % $w), [Math]::Floor($first / $w), $d[$first], $c[$first]) }
	$pixels += $c.Length
	if ($kind -ne 'none') { $alphaFiles++ }
}
foreach ($f in $expected.Keys) { if (-not $seen[$f]) { AddDiff "${f}: expected, not in textures.csv" } }
$pngs = @(Get-ChildItem -LiteralPath (Join-Path $Export 'textures') -Recurse -File -Filter *.png).Count
if ($pngs -ne $seen.Count) { AddDiff "$pngs PNGs in the folder, $($seen.Count) rows (an earlier export's files?)" }
if (-not $Keep) { Remove-Item -LiteralPath $check -Recurse }

"{0} textures in the paks, {1} names, {2} files expected; textures.csv {3} rows, {4} PNGs in the folder" -f [ExportCheck]::Textures.Count, $byName.Count, $expected.Count, $csv.Count, $pngs
"{0} files compared ({1} with alpha), {2} texels" -f $seen.Count, $alphaFiles, $pixels
if ($diffs.Count) { "$($diffs.Count) differences:"; $diffs | Select-Object -First 30 | ForEach-Object { "  $_" } } else { 'no differences' }
