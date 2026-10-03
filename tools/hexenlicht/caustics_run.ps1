# Light through water, glass and translucent things (story 6.14; docs/hexenlicht/TESTING.md,
# "Light through (6.14)"): each view paused and shot twice in one run, with pt_caustics 0 and 1
# (vk_screenshot <view>_c0 and _c1, 8 frames averaged), from 6.4's and 6.5's saves (run
# translucency_run.ps1 and water_run.ps1 with -Saves first): a test light (a dynamic sphere)
# inside village1's bay window seen from the street (its panes *44-*46; with the test pack's
# glass on rtex199, test_pack.ps1, its glass), the sun through the bay (the sky light mode), a
# test light just above demo1's, demo2's and romeric3's pools seen from above (and demo1's from
# below), the tomed sunstaff's sheath around its line light and the purifier's smoke rings.
# -Only a pattern of view names, -Extra a console command after the start (e.g. 'r_debugview
# 15; pt_num_bounce_rays 0': the direct light alone). config.cfg and hexenlicht.cfg of data1 are
# backed up and restored, the folder's own shots moved aside and back, the scripts deleted. The
# shots, the log and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [string]$Only = '', [string]$Extra = '', [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, save, what happens after the load (with the game running; ends paused when it pauses)
$light = 'vk_testlight dlight 4 3000'
$views = @(
	@('v1_bay_light', 'hl64_v1_bay_front', @('noclip', 'vk_setpos 511 2345 60', (Waits 5), $light, (Waits 2), 'vk_setpos 511 2224 50 20 90', (Waits 10))),
	@('v1_bay_sun', 'hl64_v1_bay_diag', @('r_sky_light 1', 'r_sun 1', 'r_sun_elevation 20', 'r_sun_azimuth 10')),
	@('d1_pool_light', 'hl65_d1_078_above', @('noclip', 'vk_setpos -1060 2300 -560', (Waits 5), $light, (Waits 2), 'vk_setpos -1060 2160 -522 45 90', (Waits 10))),
	@('d1_pool_light_under', 'hl65_d1_078_above', @('noclip', 'vk_setpos -1060 2300 -560', (Waits 5), $light, (Waits 2), 'vk_setpos -1060 2160 -682 10 90', (Waits 10))),
	@('d2_pool_light', 'hl65_d2_above', @('noclip', 'vk_setpos -468 -670 -120', (Waits 5), $light, (Waits 2), 'vk_setpos -468 -800 -58 45 90', (Waits 10))),
	@('r3_pool_light', 'hl65_r3_above', @('noclip', 'vk_setpos 460 880 -280', (Waits 5), $light, (Waits 2), 'vk_setpos 460 760 -226 45 90', (Waits 10))),
	@('d1_sheath', 'hl64_d1_sheath', @('impulse 25', (Waits 100), 'impulse 4', (Waits 100), '+attack', (Waits 40), 'pause', '-attack')),
	@('d1_rings', 'hl64_d1_rings', @('impulse 4', (Waits 100), '+attack', (Waits 8), '-attack', (Waits 6), 'pause'))
)
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0', 'con_notifytime 0', 'color 0 0')
if ($Extra) { $start += $Extra }
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

$blocks = @()
foreach ($v in $views) {
	if (-not (Test-Path (Join-Path $game $v[1]))) { throw "no save $($v[1]): run translucency_run.ps1 / water_run.ps1 with -Saves first" }
	$b = @('pt_caustics 0', "load $($v[1])", (Waits 20), 'vk_testlight clear') + $v[2]
	if ($v[2] -notcontains 'pause') { $b += 'pause' }
	$b += @((Waits 60), "vk_screenshot $($v[0])_c0 8", (Waits 12), 'pt_caustics 1', (Waits 60), "vk_screenshot $($v[0])_c1 8", (Waits 12),
		"echo T614_VIEW $($v[0])", 'vk_models', 'pt_caustics 0', 'pause')
	$blocks += , $b
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl614_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$files = @()
for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl614_$i.cfg"; [IO.File]::WriteAllText($f, $texts[$i]); $files += $f }

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
try {
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl614_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	# the configs and scripts first: a failed move of the shots mustn't keep them
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force } }
	foreach ($f in $files) { Remove-Item -LiteralPath $f -ErrorAction Continue }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation'
# each view's translucent models around a light (vk_models' last line)
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T614_VIEW (\S+)') {
		$view = $Matches[1]
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 12; $j++) {
			if ($log[$j] -match '(\d+) glowing beam parts \(6\.3\), (\d+) translucent ones around a light') { "${view}: $($Matches[1]) glowing beam parts, $($Matches[2]) translucent models around a light"; break }
		}
	}
}
