param([string]$Shots = '', [string]$Log = '', [string]$PortalsShots = '', [string]$PortalsLog = '', [switch]$Debug)

# Checks the screenshots and the log of special_set.ps1's test run (story
# 5.5, docs/hexenlicht/TESTING.md, "Special materials"; the data folder's
# data1\shots and debug_h2.log, or -Shots and -Log; the -portals run's with
# -PortalsShots and -PortalsLog) at 960x540 and prints PASS or FAIL per
# check (-Debug: a Debug run's, whose validation lines must be there). The
# debug views go through the composite's curve (read back through the 2.2
# power, r_srgb 0); normals are (n + 1) / 2 before it.
#  00 base color, primary (pt_reflect_refract 0)  01 kinds, primary  02 base color after the passes  03 lit
#  04 the player (chase), kinds   05 lit   06, 07 the pool: shading and geometric normals   08-10 base color, 0.2 s apart
#  11 #lowlight base color   12 lit   13 demo1's sky (lit)   14, 15 meso9 lit: #lava000 emissive 1, then 2 (reloaded)
#  16 demo1's sky after the swap (no alpha)   17 kinds after the next map load   18 egypt1's sky
#  portals: 00 keep1's sky
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' }
if (-not $Shots) { $Shots = Join-Path $Data 'data1\shots' }
if (-not $Log) { $Log = Join-Path $Data 'debug_h2.log' }
if (-not ('SpecialCheckTga' -as [type])) {
Add-Type -TypeDefinition @'
using System;
public static class SpecialCheckTga {
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
	// pixels where the red channel exceeds twice the others (the kinds view's chrome)
	public static int Reddish(byte[] b, int x0, int y0, int x1, int y1) {
		int n = 0;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { var p = Px(b, x, y); if (p[0] > 60 && p[0] > 1.6 * p[1] && p[0] > 1.6 * p[2]) n++; }
		return n;
	}
	// the mean absolute difference of two shots in a rectangle, 0-255
	public static double Diff(byte[] a, byte[] b, int x0, int y0, int x1, int y1) {
		double s = 0; int n = 0;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
			var p = Px(a, x, y); var q = Px(b, x, y);
			s += Math.Abs(p[0] - q[0]) + Math.Abs(p[1] - q[1]) + Math.Abs(p[2] - q[2]); n++;
		}
		return s / n / 3;
	}
	// the mean of a rectangle in linear light (the 2.2 power)
	public static double[] Linear(byte[] b, int x0, int y0, int x1, int y1) {
		double[] s = new double[3]; int n = 0;
		for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
			var p = Px(b, x, y); for (int c = 0; c < 3; c++) s[c] += Math.Pow(p[c] / 255.0, 2.2); n++;
		}
		return new double[] { s[0] / n, s[1] / n, s[2] / n };
	}
}
'@
}
$cache = @{}
function Shot([int]$i, [string]$dir = $Shots) {
	$k = "$dir/$i"
	if (-not $cache[$k]) { $cache[$k] = [IO.File]::ReadAllBytes((Join-Path $dir ('hexen{0:D2}.tga' -f $i))) }
	, $cache[$k]
}
$script:fails = 0; $script:checks = 0
function Check([string]$what, [bool]$ok, [string]$got) {
	if (-not $ok) { $script:fails++ }; $script:checks++
	'{0} {1}: {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $what, $got
}
function Px([int]$i, [int]$x, [int]$y) { [SpecialCheckTga]::Px((Shot $i), $x, $y) }
function Normal([int]$i, [int]$x, [int]$y) { $p = Px $i $x $y; @($p | ForEach-Object { [math]::Pow($_ / 255.0, 2.2) * 2 - 1 }) }
function Fmt($v) { ($v | ForEach-Object { '{0:F3}' -f $_ }) -join ' ' }
$lines = Get-Content -LiteralPath $Log
function After([string]$marker, [string]$pattern) {
	$i = [Array]::FindIndex($lines, [Predicate[string]]{ param($l) $l.StartsWith($marker) })
	if ($i -lt 0) { return $null }
	for ($k = $i + 1; $k -lt $lines.Count; $k++) { if ($lines[$k] -match $pattern) { return $Matches } }
	return $null
}
function Dome([string]$marker) {
	$m = After $marker '^sky light: dome ([\d.]+) ([\d.]+) ([\d.]+)'
	if ($m) { @([double]$m[1], [double]$m[2], [double]$m[3]) } else { @(-1, -1, -1) }
}
function Near($v, $e, [double]$d) { $ok = $true; for ($k = 0; $k -lt 3; $k++) { if ([math]::Abs($v[$k] - $e[$k]) -gt $d) { $ok = $false } }; $ok }

# 01 kinds (primary surfaces): the floor chrome (red), the pedestal glass (cyan), a wall regular (grey)
$f = Px 1 90 450; $p = Px 1 510 300; $w = Px 1 90 270
Check 'rtex429 kind chrome (the kinds view red)' ($f[0] -gt 1.6 * $f[1] -and $f[0] -gt 1.6 * $f[2]) ($f -join ',')
Check 'rtex426 kind glass (the kinds view cyan)' ($p[1] -gt 1.15 * $p[0] -and $p[2] -gt 1.15 * $p[0]) ($p -join ',')
Check 'rtex022 stays regular (grey)' ([math]::Abs($w[0] - $w[1]) -lt 6 -and [math]::Abs($w[1] - $w[2]) -lt 6) ($w -join ',')
# 00 against 02: the mirror and the glass show other surfaces after the passes; the wall doesn't change
$d1 = [SpecialCheckTga]::Diff((Shot 0), (Shot 2), 40, 420, 140, 480)
$d2 = [SpecialCheckTga]::Diff((Shot 0), (Shot 2), 480, 285, 540, 315)
$d3 = [SpecialCheckTga]::Diff((Shot 0), (Shot 2), 60, 250, 120, 290)
$d4 = [SpecialCheckTga]::Diff((Shot 0), (Shot 2), 200, 420, 320, 520)
Check 'the chrome floor reflects (base color after the passes)' ($d1 -gt 20) ('{0:F1}' -f $d1)
Check 'the glass pedestal is seen through and reflects' ($d2 -gt 20) ('{0:F1}' -f $d2)
Check 'a regular wall is the same after the passes' ($d3 -lt 2) ('{0:F1}' -f $d3)
Check 'the chrome gauntlet (the view weapon) reflects' ($d4 -gt 10) ('{0:F1}' -f $d4)
# 04 the player (chase camera) chrome
$n = [SpecialCheckTga]::Reddish((Shot 4), 300, 100, 660, 540); $all = 360 * 440
Check 'the player (models/paladin.mdl_0) kind chrome' ($n -gt 0.5 * $all) ('{0:P0} of the centre red' -f ($n / $all))
$inst = [bool]($lines | Select-String -Pattern 'models/paladin\.mdl .* chrome$' -Quiet)
$mc = [bool]($lines | Select-String -Pattern ", 0 whose material doesn't fit their group" -SimpleMatch -Quiet)
Check "vk_instances: the player's instance chrome; vk_models check: every kind fits" ($inst -and $mc) "instance $inst, check $mc"
$k = After 'T55_INSTANCES' '^kinds:(.*)$'
Check 'vk_world: chrome and glass triangles' ($k -and $k[1] -match 'chrome \d+' -and $k[1] -match 'glass \d+') ($(if ($k) { $k[1] } else { 'no kinds line' }))

# 06, 07 the pool (#rtex346, water): its normal map, 30 degrees towards the image's top, the same over the whole pool
$cs = foreach ($pt in @(@(480, 270), @(420, 250), @(540, 300), @(460, 320))) {
	$s = Normal 6 $pt[0] $pt[1]; $g = Normal 7 $pt[0] $pt[1]
	($s[0] * $g[0] + $s[1] * $g[1] + $s[2] * $g[2]) / [math]::Sqrt(($s[0] * $s[0] + $s[1] * $s[1] + $s[2] * $s[2]) * ($g[0] * $g[0] + $g[1] * $g[1] + $g[2] * $g[2]))
}
Check "the water's normal map (#rtex346_n): tilted 30 degrees" (($cs | Where-Object { [math]::Abs($_ - 0.866) -gt 0.05 }).Count -eq 0) `
	("cos {0}" -f (($cs | ForEach-Object { '{0:F3}' -f $_ }) -join ' '))
# the pool is two coincident faces (up and down) and primary rays hit either: its shading normal the same
# over the pool (5.5; the mirrored frame on the back face speckled it with a second tilt: a cosine can't tell them)
$c = Px 6 480 270
$n = [SpecialCheckTga]::Count((Shot 6), 400, 230, 560, 330, $c[0], $c[1], $c[2], 6)
Check "the water's normal map alike on the pool's two coincident faces" ($n -gt 0.97 * 160 * 100) ('{0:P1} of the pool as its centre' -f ($n / (160 * 100)))
# 08 the pool's albedo; 08-10 the runes' frames
$p = Px 8 480 270
Check "the water's albedo (#rtex346, the cyan checker)" ((Near $p @(0, 255, 255) 40) -or (Near $p @(0, 60, 60) 40) -or (Near $p @(0, 96, 96) 40)) ($p -join ',')
$colors = @(@('red', 255, 0, 0), @('green', 0, 255, 0), @('blue', 0, 0, 255), @('yellow', 255, 255, 0), @('magenta', 255, 0, 255))
$seen = foreach ($i in 8, 9, 10) {
	$best = ''; $most = 0
	foreach ($c in $colors) { $n = [SpecialCheckTga]::Count((Shot $i), 0, 100, 960, 540, $c[1], $c[2], $c[3], 30); if ($n -gt $most) { $most = $n; $best = $c[0] } }
	"$best ($most)"
}
$names = $seen | ForEach-Object { ($_ -split ' ')[0] }
Check 'the animated +0rune1..+4rune1: each frame its own file, 0.2 s apart' ($names[0] -ne $names[1] -and $names[1] -ne $names[2] -and ($names | Where-Object { $_ }).Count -eq 3) ($seen -join ', ')
# 11 the translucent #lowlight's albedo; 12 still seen through
$n = [SpecialCheckTga]::Count((Shot 11), 300, 250, 660, 450, 40, 255, 40, 80) + [SpecialCheckTga]::Count((Shot 11), 300, 250, 660, 450, 0, 90, 0, 60)
Check "#lowlight's albedo (the green checker)" ($n -gt 0.3 * 360 * 200) ('{0:P0}' -f ($n / (360 * 200)))

# 13 demo1's sky: the file's red front over its blue back at 0.67, the front's checker of holes
$red = [SpecialCheckTga]::Count((Shot 13), 0, 60, 960, 360, 171, 0, 84, 45); $blue = [SpecialCheckTga]::Count((Shot 13), 0, 60, 960, 360, 0, 0, 255, 45); $all = 960 * 300
Check "demo1's sky: its file's layers, the front's alpha" ($red -gt 0.3 * $all -and $blue -gt 0.3 * $all -and $red + $blue -gt 0.85 * $all) ('front {0:P0}, back {1:P0}' -f ($red / $all), ($blue / $all))
$d = Dome 'T55_SKY_A'
Check "the dome (sky light) from the file's layers: 0.5 of L(0.67) and of L(0.33) + 0.5" (Near $d @(0.2072, 0, 0.5437) 0.003) (Fmt $d)
# 16 after the swap, the file without alpha: the original's transparency (index 0: 5405 of 16384 texels)
$red = [SpecialCheckTga]::Count((Shot 16), 0, 60, 960, 360, 171, 0, 84, 45); $blue = [SpecialCheckTga]::Count((Shot 16), 0, 60, 960, 360, 0, 0, 255, 45)
Check "the sky's file without alpha: the original's holes (about a third)" ($blue -gt 0.15 * $all -and $blue -lt 0.5 * $all -and $red + $blue -gt 0.85 * $all) ('front {0:P0}, back {1:P0}' -f ($red / $all), ($blue / $all))
$d = Dome 'T55_SKY_B'
Check "the dome after the reload: the original transparency's 0.330" (Near $d @(0.2777, 0, 0.3884) 0.003) (Fmt $d)
# 18 egypt1: sky000 (the plain name), no alpha, egypt's front has no holes: yellow over green at 0.67
$n = [SpecialCheckTga]::Count((Shot 18), 0, 60, 960, 360, 171, 255, 0, 45)
Check "egypt1's sky000: the plain name's file, the original's (opaque) front" ($n -gt 0.85 * $all) ('{0:P0}' -f ($n / $all))
$m = After 'T55_SKY_EGYPT' '^sky sky000: r_skyalpha [\d.]+, average color \(linear\) ([\d.]+) ([\d.]+) ([\d.]+)'
$e = if ($m) { @([double]$m[1], [double]$m[2], [double]$m[3]) } else { @(-1, -1, -1) }
Check "egypt1's average: L(0.67), 1, 0" (Near $e @(0.4144, 1, 0) 0.003) (Fmt $e)

# 14, 15 meso9's lava: its lights blue (its file's), then twice as bright (.mat emissive 2, reloaded)
$l1 = After 'T55_LAVA_1' '\*lava000 \d+ \(color ([\d.]+) ([\d.]+) ([\d.]+) its file''s, x ([\d.]+)\)'
$l2 = After 'T55_LAVA_2' '\*lava000 \d+ \(color ([\d.]+) ([\d.]+) ([\d.]+) its file''s, x ([\d.]+)\)'
Check "#lava000's lights: its file's color (blue), x 1, then x 2" ($l1 -and $l2 -and [double]$l1[3] -gt 5 * [double]$l1[1] -and $l1[4] -eq '1' -and $l2[4] -eq '2') `
	($(if ($l1 -and $l2) { "$($l1[1]) $($l1[2]) $($l1[3]) x $($l1[4]), then x $($l2[4])" } else { 'no lava lines' }))
$a = [SpecialCheckTga]::Linear((Shot 14), 760, 100, 960, 300); $b = [SpecialCheckTga]::Linear((Shot 15), 760, 100, 960, 300)
Check "the lava's light on meso9's wall: its blue share about doubles, the map lights' red stays" ($b[2] -gt 1.5 * $a[2] -and [math]::Abs($b[0] / $a[0] - 1) -lt 0.05) `
	('blue {0:F4} -> {1:F4}, red {2:F4} -> {3:F4}' -f $a[2], $b[2], $a[0], $b[0])

# the kind change: reported by the reload, applied at the next map load (17)
$kc = [bool]($lines | Select-String -Pattern 'the kind of 1 world texture (rtex426) changed: it applies at the next map load' -SimpleMatch -Quiet)
$p = Px 17 510 300; $f = Px 17 90 450
Check "rtex426's kind changed: the reload says so, the next map load applies it (grey), rtex429 still chrome" `
	($kc -and [math]::Abs($p[0] - $p[1]) -lt 6 -and [math]::Abs($p[1] - $p[2]) -lt 6 -and $f[0] -gt 1.6 * $f[1]) ("reported $kc; pedestal $($p -join ','), floor $($f -join ',')")

# the problems
foreach ($t in @(
	@("+1rune2's kind: not its animation's first frame", "+1rune2.mat line 2: kind chrome: an animated texture's kind is its first frame's"),
	@('meso: sky001~4893.dds (BC7) refused', 'sky001~4893.dds: BC7: the sky''s layers are split and edited at load'),
	@('sky000_n.png: the sky takes only its albedo', 'sky000_n.png: the sky is unlit, only its albedo applies'))) {
	Check $t[0] ([bool]($lines | Select-String -Pattern $t[1] -SimpleMatch -Quiet)) ''
}
if ($Debug) {	# a Debug build's summary at quit: missing if it ended early
	$v = $lines | Select-String -Pattern 'Vulkan validation:' | Select-Object -Last 1
	Check 'Debug validation' ($v -and $v.Line -match '0 errors, 0 warnings') $(if ($v) { $v.Line } else { 'no validation line' })
}

# the -portals run: keep1's sky (sky001~6566, the file without alpha since the swap)
if ($PortalsShots) {
	$red = [SpecialCheckTga]::Count((Shot 0 $PortalsShots), 320, 60, 640, 360, 171, 0, 84, 45); $blue = [SpecialCheckTga]::Count((Shot 0 $PortalsShots), 320, 60, 640, 360, 0, 0, 255, 45)
	$all = 320 * 300	# the sky between keep1's walls
	Check "keep1 (-portals): demo1's sky file (sky001~6566), the original's holes" ($red -gt 0.3 * $all -and $blue -gt 0.15 * $all -and $red + $blue -gt 0.85 * $all) ('front {0:P0}, back {1:P0}' -f ($red / $all), ($blue / $all))
}
if ($PortalsLog) {
	$v = Get-Content -LiteralPath $PortalsLog | Select-String -Pattern 'Vulkan validation:' | Select-Object -Last 1
	if ($Debug) { Check 'Debug validation (-portals)' ($v -and $v.Line -match '0 errors, 0 warnings') $(if ($v) { $v.Line } else { 'no validation line' }) }
}
"{0} checks, {1} failed" -f $script:checks, $script:fails
