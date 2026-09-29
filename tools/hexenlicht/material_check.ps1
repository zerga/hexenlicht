param([string]$Shots = '')

# Checks the screenshots of material_set.ps1's test run (story 5.3,
# docs/hexenlicht/TESTING.md, "Materials"; the data folder's data1\shots, or
# -Shots) at points of demo1's start at 960x540, where its probe found the
# textures, and prints PASS or FAIL per check. The debug views go through
# the composite's curve, so values are read back through the 2.2 power
# (r_srgb 0); normals are (n + 1) / 2 before it.
#  00 base color   01 shading normals   02 geometric normals   03 roughness, metallic, specular
#  04 lit          05, 06 the same with r_materials 0          07, 08 the player (chase), colors 0 0 and 4 4
#  09, 10 the ice mace, base color and lit   11, 12 after the hot reload   13-15 its hits (sprites), base color
#  16, 17 base color while rtex388~6280.png is being written, then when written
#  18, 19 meso9 lit and base color (lava)    20-22 castle4 lit: r_materials 1, 0, r_emissive_models 0 (a torch's flame)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Shots) {
	$Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' }
	$Shots = Join-Path $Data 'data1\shots'
}
if (-not ('MaterialCheckTga' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class MaterialCheckTga {
	public static int[] Px(byte[] b, int x, int y) {
		int idlen = b[0], w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bpp = b[16] / 8; bool top = (b[17] & 0x20) != 0;
		int row = top ? y : (h - 1 - y); int o = 18 + idlen + (row * w + x) * bpp;
		return new int[] { b[o + 2], b[o + 1], b[o] };
	}
	// pixels of a rectangle within d (sum of channel differences) of a color
	public static int Count(byte[] b, int x0, int y0, int x1, int y1, int r, int g, int bl, int d) {
		int n = 0;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
			var p = Px(b, x, y);
			if (Math.Abs(p[0] - r) + Math.Abs(p[1] - g) + Math.Abs(p[2] - bl) <= d) n++;
		}
		return n;
	}
	// mean of a rectangle, 0-255 per channel
	public static double[] Mean(byte[] b, int x0, int y0, int x1, int y1) {
		double r = 0, g = 0, bl = 0; int n = 0;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { var p = Px(b, x, y); r += p[0]; g += p[1]; bl += p[2]; n++; }
		return new double[] { r / n, g / n, bl / n };
	}
}
'@
}
$cache = @{}
function Shot([int]$i) {
	if (-not $cache[$i]) { $cache[$i] = [IO.File]::ReadAllBytes((Join-Path $Shots ('hexen{0:D2}.tga' -f $i))) }
	, $cache[$i]	# as one array, not its bytes one by one
}
$script:fails = 0
function Check([string]$what, [bool]$ok, [string]$got) {
	if (-not $ok) { $script:fails++ }
	'{0} {1}: {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $what, $got
}
function Lin([int]$c) { [math]::Pow($c / 255.0, 2.2) }
function Value([int]$i, [int]$x, [int]$y) { $p = [MaterialCheckTga]::Px((Shot $i), $x, $y); @((Lin $p[0]), (Lin $p[1]), (Lin $p[2])) }
function Normal([int]$i, [int]$x, [int]$y) { $v = Value $i $x $y; @(($v[0] * 2 - 1), ($v[1] * 2 - 1), ($v[2] * 2 - 1)) }
function Fmt($v) { ($v | ForEach-Object { '{0:F3}' -f $_ }) -join ' ' }

# 00 base color
$p = [MaterialCheckTga]::Px((Shot 0), 90, 270)
$near = { param($p, $c, $d) ([math]::Abs($p[0] - $c[0]) + [math]::Abs($p[1] - $c[1]) + [math]::Abs($p[2] - $c[2])) -le $d }
Check 'rtex022 albedo (PNG): the checker' ((& $near $p @(230, 120, 40) 30) -or (& $near $p @(30, 40, 140) 30)) ($p -join ',')
$p = [MaterialCheckTga]::Px((Shot 0), 680, 390)
Check 'rtex430: the ~6995 name before the plain one (red)' (& $near $p @(255, 0, 0) 30) ($p -join ',')
$n = [MaterialCheckTga]::Count((Shot 0), 180, 380, 360, 540, 40, 220, 40, 60) + [MaterialCheckTga]::Count((Shot 0), 180, 380, 360, 540, 220, 40, 220, 60)
Check 'the gauntlet skin (models/gauntlet.mdl_0)' ($n -gt 3000) "$n green or magenta pixels"

# 01, 02 normal maps: 30 degrees towards the image's top, up the wall
foreach ($t in @(@('rtex021 (PNG)', 390, 210), @('rtex022 (BC5 DDS)', 90, 270))) {
	$s = Normal 1 $t[1] $t[2]; $g = Normal 2 $t[1] $t[2]
	$dot = ($s[0] * $g[0] + $s[1] * $g[1] + $s[2] * $g[2]) / [math]::Sqrt(($s[0] * $s[0] + $s[1] * $s[1] + $s[2] * $s[2]) * ($g[0] * $g[0] + $g[1] * $g[1] + $g[2] * $g[2]))
	Check "$($t[0]) normal map: leans up the wall" ([math]::Abs($s[2] - $g[2] - 0.5) -lt 0.08 -and [math]::Abs($dot - 0.866) -lt 0.05) ("shading {0}, geometric {1}, cos {2:F3}" -f (Fmt $s), (Fmt $g), $dot)
}
$s = Normal 1 870 90; $g = Normal 2 870 90
Check 'rtex013: its BC7 normal map left out (flat)' ([math]::Abs($s[0] - $g[0]) + [math]::Abs($s[1] - $g[1]) + [math]::Abs($s[2] - $g[2]) -lt 0.03) ("shading {0}, geometric {1}" -f (Fmt $s), (Fmt $g))

# 03 roughness, metallic, specular
foreach ($t in @(
	@('rtex021 _r 128 alone', 390, 210, 0.502, 0, 1),
	@('rtex040 _orm (G 64, B 255)', 810, 270, 0.251, 1, 1),
	@('rtex429 _m 255 alone, .mat metallic 0.5', 90, 450, 1, 0.5, 0.5),
	@('rtex388 .mat roughness 0.3 metallic 0.7 specular 0.5', 870, 450, 0.3, 0.7, 0.85),
	@('rtex430 _r 77 from the plain name', 680, 390, 0.302, 0, 1),
	@('rtex038 _r.dds refused: the defaults', 870, 150, 1, 0, 0),
	@('rtex022 albedo and normal only: matte', 90, 270, 1, 0, 0))) {
	$v = Value 3 $t[1] $t[2]
	$ok = [math]::Abs($v[0] - $t[3]) -lt 0.03 -and [math]::Abs($v[1] - $t[4]) -lt 0.03 -and [math]::Abs($v[2] - $t[5]) -lt 0.03
	Check $t[0] $ok ("{0}, expected {1} {2} {3}" -f (Fmt $v), $t[3], $t[4], $t[5])
}

# 04 against 06 (r_materials 0): the emission
$a = [MaterialCheckTga]::Mean((Shot 4), 470, 280, 550, 320); $b = [MaterialCheckTga]::Mean((Shot 6), 470, 280, 550, 320)
Check 'rtex426 _e (yellow) and .mat emissive 0.5 glow (clipping towards white)' ($a[0] -gt 3 * $b[0] -and $a[2] -lt $a[1] - 20) ("lit {0:F0} {1:F0} {2:F0}, without files {3:F0} {4:F0} {5:F0}" -f $a[0], $a[1], $a[2], $b[0], $b[1], $b[2])
$a = [MaterialCheckTga]::Mean((Shot 4), 730, 80, 770, 100); $b = [MaterialCheckTga]::Mean((Shot 6), 730, 80, 770, 100)
Check 'rtex028 .mat emissive 1: its albedo glows' (($a[0] + $a[1] + $a[2]) -gt 2 * ($b[0] + $b[1] + $b[2])) ("lit {0:F0} {1:F0} {2:F0}, without files {3:F0} {4:F0} {5:F0}" -f $a[0], $a[1], $a[2], $b[0], $b[1], $b[2])
# 05 r_materials 0: nothing of the set (compare it with main's shot for identity: tga_diff.ps1)
$p = [MaterialCheckTga]::Px((Shot 5), 680, 390)
Check 'r_materials 0: rtex430 original' (-not (& $near $p @(255, 0, 0) 60)) ($p -join ',')

# 07, 08 the player
$c7 = [MaterialCheckTga]::Count((Shot 7), 0, 70, 960, 540, 255, 230, 0, 60) + [MaterialCheckTga]::Count((Shot 7), 0, 70, 960, 540, 120, 0, 200, 60)
$c8 = [MaterialCheckTga]::Count((Shot 8), 0, 70, 960, 540, 255, 230, 0, 60) + [MaterialCheckTga]::Count((Shot 8), 0, 70, 960, 540, 120, 0, 200, 60)
Check 'the player: the replaced albedo with colors 0 0, the original with 4 4' ($c7 -gt 20000 -and $c8 -lt 500) "$c7 and $c8 pixels of the checker"

# 09, 11 the ice mace (EF_HOLEY), and the hot reload
$c9 = [MaterialCheckTga]::Count((Shot 9), 380, 310, 600, 540, 0, 255, 255, 80)
$c11 = [MaterialCheckTga]::Count((Shot 11), 380, 310, 600, 540, 0, 255, 255, 80)
Check 'the ice mace: its albedo (the original holes), then one with its own (the handle goes)' ($c9 -gt 5000 -and $c11 -lt 0.8 * $c9) "$c9 then $c11 cyan pixels"
$p = [MaterialCheckTga]::Px((Shot 11), 680, 390)
Check 'the hot reload: rtex430~6995 deleted, the plain name (blue)' (& $near $p @(0, 0, 255) 40) ($p -join ',')

# 13-15 the sprite, magenta where the original has coverage
$m = 0; foreach ($i in 13..15) { $m = [math]::Max($m, [MaterialCheckTga]::Count((Shot $i), 0, 70, 960, 540, 255, 0, 255, 90)) }
Check 'the ice mace hits (models/icehit.spr): the replaced frames' ($m -gt 50) "at most $m magenta pixels in a shot"

# 16, 17 a file being written: refused (the game goes on), then read
$p16 = [MaterialCheckTga]::Px((Shot 16), 870, 450); $p17 = [MaterialCheckTga]::Px((Shot 17), 870, 450)
Check 'rtex388~6280.png while written: refused, then its yellow' ((-not (& $near $p16 @(255, 220, 60) 60)) -and (& $near $p17 @(255, 220, 60) 30)) ("{0}, then {1}" -f ($p16 -join ','), ($p17 -join ','))
$log = Join-Path (Split-Path (Split-Path $Shots)) 'debug_h2.log'
if (Test-Path $log) {
	$refused = [bool](Select-String -LiteralPath $log -Pattern 'rtex388~6280.png: can''t be opened' -SimpleMatch -Quiet)
	Check 'the log: the file being written refused' $refused $log
}

# 18, 19 meso9's lava
$l = [MaterialCheckTga]::Mean((Shot 19), 400, 325, 560, 340)
Check 'meso9 #lava000: the blue albedo' ($l[2] -gt 2 * $l[0]) ("{0:F0} {1:F0} {2:F0}" -f $l[0], $l[1], $l[2])
$l = [MaterialCheckTga]::Mean((Shot 18), 400, 325, 560, 340)
Check 'meso9 #lava000: emits it' ($l[2] -gt 2 * $l[0] -and $l[2] -gt 150) ("{0:F0} {1:F0} {2:F0}" -f $l[0], $l[1], $l[2])

# 20-22 castle4's torch: the flame's _e (green), not without files, nor with r_emissive_models 0
$g = foreach ($i in 20..22) { [MaterialCheckTga]::Count((Shot $i), 535, 195, 570, 235, 0, 255, 0, 200) }
Check "castle4's flame: its _e (green) instead of the fake emissive" ($g[0] -gt 10 -and $g[1] -eq 0) "$($g[0]) green pixels, $($g[1]) without files"
Check "castle4's flame: r_emissive_models 0 turns the _e off" ($g[2] -eq 0) "$($g[2]) green pixels"

if ($script:fails) { "$script:fails checks failed" } else { 'all checks passed' }
