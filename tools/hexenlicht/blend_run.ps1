# View blends (story 6.6; docs/hexenlicht/TESTING.md, "View blends (6.6)"): glh2 and
# Hexenlicht from demo1's start (map demo1: the same view in both), shots named by step.
#  1. Paused pairs: each of GL's blends as v_cshift's tint (the contents shift in the air, the
#     same V_CalcBlend): the damage flash's three colors at 150/255, the bonus flash, the
#     power-up tints (invisibility, invincibility, frozen, stoned at the 80 V_CalcBlend gives
#     it), the dark and white flashes half faded, and none (the noise between two frames);
#     each shot with gl_polyblend 0 (<step>_0) and 1 (<step>_1). The summary checks every
#     pair against GL's blend function in the 8-bit values: out = c (1 - a) + 255 k a
#     (gamma 1), per engine: the mean and largest difference, the share over 2 levels and the
#     mean signed difference (none: the noise alone), and for contrast the mean difference
#     of the same blend in linear light. hud_1: the status bar (viewsize 100) under a damage
#     tint.
#  2. Live, the game running (host_framerate 0.02: the flashes fade 2 levels of 255 a
#     frame): bf (the bonus flash), df and wf (the hydra's dark flash, the white flash) a
#     frame and later after the command, the Icon of the Defender (invincibility) and the
#     invisibility artifact. glh2's screenshot reads its back buffer, a frame or more older
#     than the next frame Hexenlicht's shows: its _01 shots show no flash yet.
#  3. Damage: a new demo1 without god, an archer created in front (80 units ahead of the
#     view), a shot every 5 frames for 3 s (dmg_NN): an arrow's flash fades in about 10.
#     Which shots catch a hit differs between the engines and runs (the archer's choices).
#  4. Hexenlicht only: under demo1's pool (noclip, vk_setpos), the bonus flash without the
#     water's brown (GL's contents shift, left out: the medium, DECISIONS X22), r_dumpscene's
#     blend in the log.
# -Exe hexenlicht, glh2 or both; -Release; -Bin another build's folder for Hexenlicht.
# config.cfg and hexenlicht.cfg of data1 are backed up and restored around each run, the
# folder's own numbered shots moved aside and back, the scripts deleted. The shots go to
# -Out\<Tag>\<exe>, the logs and the summary to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [ValidateSet('hexenlicht', 'glh2', 'both')][string]$Exe = 'both',
      [string]$Bin = '', [switch]$Release, [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
$exes = if ($Exe -eq 'both') { 'hexenlicht', 'glh2' } else { , $Exe }
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, v_cshift's r g b percent (GL's numbers, view.c)
$tints = @(
	@('none', '0 0 0 0'),
	@('dmg_armor', '200 100 100 150'), @('dmg_mixed', '220 50 50 150'), @('dmg_blood', '255 0 0 150'),
	@('bonus', '215 186 69 50'), @('invisibility', '100 100 100 100'), @('invincibility', '255 255 0 30'),
	@('frozen', '20 70 255 65'), @('stoned', '205 205 205 80'), @('dark_half', '0 0 0 128'), @('white_half', '255 255 255 128')
)

# a step's items: a console command, or '@<name>' for a shot (the next frame), 'hl:' for Hexenlicht only
$items = @('map demo1', (Waits 150), 'god', 'notarget', 'impulse 43', (Waits 30), 'pause', (Waits 10))
foreach ($t in $tints) {
	$items += @("v_cshift $($t[1])", 'gl_polyblend 0', (Waits 8), "@$($t[0])_0", (Waits 3), 'gl_polyblend 1', (Waits 8), "@$($t[0])_1", (Waits 3))
}
$items += @('viewsize 100', 'v_cshift 255 0 0 150', (Waits 8), '@hud_1', (Waits 3), 'viewsize 130', 'v_cshift 0 0 0 0', (Waits 3), 'pause')
$items += @((Waits 10), 'bf', (Waits 1), '@bf_01', (Waits 9), '@bf_10', (Waits 40))
foreach ($f in 'df', 'wf') { $items += @($f, (Waits 1), "@${f}_01", (Waits 59), "@${f}_60", (Waits 60), "@${f}_120", (Waits 30)) }
$items += @('impulse 114', (Waits 10), '@invincibility', 'impulse 107', (Waits 10), '@invisibility', (Waits 5))
$items += @('map demo1', (Waits 150), 'create monster_archer', (Waits 10))
for ($i = 0; $i -lt 30; $i++) { $items += @(('@dmg_{0:d2}' -f $i), (Waits 4)) }
$items += @('hl:map demo1', "hl:$(Waits 150)", 'hl:god', 'hl:notarget', 'hl:noclip', 'hl:vk_setpos -1060 2300 -682 -25 0', "hl:$(Waits 30)",
	    'hl:bf', "hl:$(Waits 1)", 'hl:@water_bf_01', 'hl:r_dumpscene', "hl:$(Waits 30)", 'hl:@water_none', 'hl:r_dumpscene')

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'gamma 1', 'scr_centertime 0', 'r_drawviewmodel 1', 'gl_polyblend 1', 'v_cshift 0 0 0 0',
	   'playerclass 1', 'skill 1')
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

$dst = Join-Path $Out $Tag
New-Item -ItemType Directory -Force $dst | Out-Null
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$summary = @()
foreach ($e in $exes) {
	$lines = @(); $names = @()
	foreach ($it in $items) {
		if ($it -like 'hl:*') { if ($e -ne 'hexenlicht') { continue }; $it = $it.Substring(3) }
		if ($it -like '@*') { $lines += 'screenshot'; $names += $it.Substring(1) } elseif ($it) { $lines += $it }
	}
	# scripts of at most ~6 KB (the command buffer holds 8), each exec'ing the next
	$files = @(); $cur = @(); $size = 0; $all = $start + $lines
	foreach ($l in $all) {
		if ($size + $l.Length -gt 6000) { $files += , $cur; $cur = @(); $size = 0 }
		$cur += $l; $size += $l.Length + 2
	}
	$files += , ($cur + $end)
	$paths = @()
	for ($i = 0; $i -lt $files.Count; $i++) {
		$text = ($files[$i] -join "`r`n") + $(if ($i + 1 -lt $files.Count) { "`r`nexec hl66_$($i + 1).cfg" } else { '' }) + "`r`n"
		$p = Join-Path $game "hl66_$i.cfg"; [IO.File]::WriteAllText($p, $text); $paths += $p
	}
	$odir = Join-Path $dst $e; New-Item -ItemType Directory -Force $odir | Out-Null
	$bk = Join-Path $dst "configs_$e"; New-Item -ItemType Directory -Force $bk | Out-Null
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
	$aside = Join-Path $dst "aside_$e"; New-Item -ItemType Directory -Force $aside | Out-Null
	Get-ChildItem $shots -Filter 'hexen*.tga' | Move-Item -Destination $aside
	try {
		$a = @{ Exe = $e; Cfg = 'hl66_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
		if ($Bin -and $e -eq 'hexenlicht') { $a.Bin = $Bin }
		if ($Release) { $a.Release = $true }
		$r = & $run @a
		if ($r -ne 'exit 0') { throw "$e ended with: $r" }
		$got = @(Get-ChildItem $shots -Filter 'hexen*.tga' | Sort-Object Name)
		if ($got.Count -ne $names.Count) { throw "$e wrote $($got.Count) shots, expected $($names.Count)" }
		for ($i = 0; $i -lt $names.Count; $i++) { Move-Item $got[$i].FullName (Join-Path $odir "$($names[$i]).tga") -Force }
		Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst "$e.log") -Force
	} finally {
		foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force } }
		foreach ($p in $paths) { [IO.File]::Delete($p) }
		Get-ChildItem $shots -Filter 'hexen*.tga' | ForEach-Object { [IO.File]::Delete($_.FullName) }
		Get-ChildItem $aside -File | Move-Item -Destination $shots -Force
		if (-not (Get-ChildItem $aside)) { [IO.Directory]::Delete($aside) }
	}
	$summary += "${e}: $($names.Count) shots; " + ((Get-Content (Join-Path $dst "$e.log") | Where-Object { $_ -match '^Vulkan validation:' }) -join ' ')
}

# each pair against GL's blend function: out = c (1 - a) + 255 k a, in the 8-bit values
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class BlendCheck {
    // { mean |diff|, max |diff|, share of channel values over 2, mean signed diff (a bias),
    // mean |diff| of the same blend in linear light (2.2), for contrast } of b against a blended
    public static double[] Check(string fa, string fb, double r, double g, double bl, double alpha) {
        byte[] a = File.ReadAllBytes(fa), b = File.ReadAllBytes(fb);
        int w = a[12] | (a[13] << 8), h = a[14] | (a[15] << 8), o = 18 + a[0];
        if (a[2] != 2 || a[16] != 24 || b.Length != a.Length) throw new Exception("not two 24-bit TGAs of one size: " + fa);
        double[] k = { bl, g, r };	// BGR
        double sum = 0, bias = 0, lin = 0; int max = 0; long over = 0, n = (long)w * h * 3;
        for (long i = 0; i < n; i++) {
            double c = a[o + i], kc = k[i % 3];
            double e = Math.Round(c * (1 - alpha) + 255 * kc * alpha);
            double el = Math.Round(255 * Math.Pow(Math.Pow(c / 255, 2.2) * (1 - alpha) + Math.Pow(kc, 2.2) * alpha, 1 / 2.2));
            int d = (int)Math.Abs(e - b[o + i]);
            sum += d; bias += b[o + i] - e; lin += Math.Abs(el - b[o + i]);
            if (d > max) max = d; if (d > 2) over++;
        }
        return new double[] { sum / n, max, (double)over / n, bias / n, lin / n };
    }
}
'@
foreach ($e in $exes) {
	foreach ($t in $tints) {
		$v = $t[1] -split ' ' | ForEach-Object { [double]$_ }
		$fa = Join-Path $dst "$e\$($t[0])_0.tga"; $fb = Join-Path $dst "$e\$($t[0])_1.tga"
		$c = [BlendCheck]::Check($fa, $fb, $v[0] / 255, $v[1] / 255, $v[2] / 255, [Math]::Min($v[3] / 255, 1))
		$summary += '{0} {1}: mean {2:f2}, max {3}, over 2: {4:p2}, bias {5:f2} (blended in linear light: mean {6:f2})' -f $e, $t[0], $c[0], $c[1], $c[2], $c[3], $c[4]
	}
}
$summary | Set-Content (Join-Path $dst 'summary.txt')
$summary
