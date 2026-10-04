# The player's own model in shadows and reflections (story 6.11; docs/hexenlicht/TESTING.md,
# "Own model (6.11)"): each view loads its map with the class it names, goes to the place
# (vk_setpos: the player's origin and view, with noclip: a fall would level the view; the
# views' z is a standing origin),
# runs the view's commands, pauses and shoots twice, with r_viewer_model 0 (no own model,
# the image before 6.11) and 1 (vk_screenshot <view>_v0 and _v1, 8 frames averaged); then
# prints vk_models and its check, and cl.light_level (r_dumpscene's line) with both settings.
# -Explore instead shoots the places of $places looking down (pitch 40) in four directions,
# to find where the own model's shadow shows. -Mirror shoots $mirrorViews instead, with
# the cathedral's and demo1's start floors mirrors (material files in data1\textures,
# removed after). -Off keeps r_viewer_model 0 throughout, for the shots to compare with a
# build before 6.11 (-Bin). -Cost measures each view with vk_benchmark 1,
# the profiler's averages of 120 paused frames, r_viewer_model 0 and 1 alternating twice
# (Release, e.g. -Width 1920 -Height 1080). -Only a pattern of view names. config.cfg and
# hexenlicht.cfg of data1 are backed up and restored, the folder's own shots moved aside and
# back, the scripts deleted. The shots, the log and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [string]$Only = '', [switch]$Explore, [switch]$Mirror, [switch]$Off, [switch]$Cost, [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, class (1 Paladin, 2 Crusader, 3 Necromancer, 4 Assassin), commands after the
# load (before vk_setpos), the place (x y z pitch yaw), commands before the pause
$sun = 'r_sky_light 1; r_sun 1'	# per-map cvars: after the load
$cloak = "$((@('impulse 40', (Waits 5)) * 9) -join ';');$(Waits 250)"	# the level cheat to 10: the Assassin cloaks after 2 s still where cl.light_level <= 100
$views = @(
	# shadows: the sun (azimuth 45, elevation 45: the shadow along yaw 225), map lights
	@('e1_sun', 'egypt1', 2, $sun, '-839.8 456.2 -6.6 70 225', ''),
	@('e1_sun_paladin', 'egypt1', 1, $sun, '-839.8 456.2 -6.6 70 225', ''),
	@('e1_sun_necro', 'egypt1', 3, $sun, '-839.8 456.2 -6.6 70 225', ''),
	@('e1_sun_assassin', 'egypt1', 4, $sun, '-839.8 456.2 -6.6 70 225', ''),
	@('cath', 'cath', 2, '', '-440 536 2 40 90', ''),
	@('cath_chase', 'cath', 2, 'chase_active 1', '-440 536 2 40 90', ''),
	@('v1_start', 'village1', 2, 'chase_active 0', '200 -784 30 40 270', ''),
	@('v1_turn', 'village1', 2, '', '200 -784 30 40 270', "cl_yawspeed 100;+right;$(Waits 20);-right;cl_yawspeed 140;$(Waits 10)"),	# 2 degrees a frame: 40 to the right
	@('d1_arch', 'demo1', 2, '', '-1624 -696 -136 40 180', ''),
	@('d1_arch_torch', 'demo1', 2, "impulse 43;$(Waits 30);impulse 100;$(Waits 60)", '-1624 -696 -136 40 180', ''),
	@('d1_arch_assassin', 'demo1', 4, '', '-1624 -696 -136 40 180', ''),
	@('d1_arch_cloak', 'demo1', 4, '', '-1624 -696 -136 40 180', $cloak),
	# reflections and refractions: above pools looking down (noclip: floating), under them looking up
	@('d1_pool_down', 'demo1', 2, '', '-1060 2300 -522 80 0', ''),
	@('d1_pool_up', 'demo1', 2, '', '-1060 2300 -682 -80 0', ''),
	@('d1_pool_stone', 'demo1', 2, "impulse 43;$(Waits 30);impulse 114;$(Waits 60)", '-1060 2300 -522 80 0', ''),
	@('d2_pool_down', 'demo2', 2, '', '-468 -670 -58 80 0', ''),
	@('d2_pool_up', 'demo2', 2, '', '-468 -670 -218 -80 0', ''),
	@('r3_pool_down', 'romeric3', 2, '', '460 880 -226 80 0', '')
)
# -Explore's places: name, map, setup, x y z (the player's origin, standing)
$places = @(
	@('demo1_start', 'demo1', '', '-918 -2034 0'),
	@('demo1_yard', 'demo1', '', '-1552 1096 0'),
	@('demo1_arch', 'demo1', '', '-1624 -696 -136'),
	@('village1_start', 'village1', '', '200 -784 30'),
	@('castle4_start', 'castle4', '', '-576 16 128'),
	@('cath_start', 'cath', '', '-440 536 2'),
	@('romeric1_start', 'romeric1', '', '0 1472 56'),
	@('egypt1_sun', 'egypt1', $sun, '-839.8 456.2 -6.6')
)
# -Mirror: the floors under the cathedral's start (rtex074, the marble) and demo1's (rtex388,
# the grass: dark enough for the Assassin to cloak) mirrors (material files: chrome, roughness
# 0.01, a light albedo; written into data1\textures for the run, refused if it has files)
$mirrorTextures = 'rtex074', 'rtex388'
$mirrorViews = @(
	@('cath_mirror_down', 'cath', 2, '', '-440 536 2 85 90', ''),
	@('cath_mirror_60', 'cath', 2, '', '-440 536 2 60 90', ''),
	@('cath_mirror_torch', 'cath', 2, "impulse 43;$(Waits 30);impulse 100;$(Waits 60)", '-440 536 2 85 90', ''),
	@('cath_mirror_stone', 'cath', 2, "impulse 114;$(Waits 60)", '-440 536 2 85 90', ''),
	@('cath_mirror_paladin', 'cath', 1, '', '-440 536 2 85 90', ''),
	@('cath_mirror_tint', 'cath', 4, "impulse 43;$(Waits 30);impulse 114;$(Waits 60)", '-440 536 2 85 90', ''),	# the Assassin's invincibility: colormap 140
	@('d1_mirror_assassin', 'demo1', 4, '', '-918 -2034 0 85 45', ''),
	@('d1_mirror_cloak', 'demo1', 4, '', '-918 -2034 0 85 45', $cloak)
)
if ($Mirror) { $views = $mirrorViews }
if ($Explore) {
	$views = @()
	foreach ($e in $places) {
		foreach ($yaw in 0, 90, 180, 270) { $views += , @("x_$($e[0])_$yaw", $e[1], 2, $e[2], "$($e[3]) 40 $yaw", '') }
	}
}
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

# -Off: r_viewer_model 0 throughout (both shots), for the comparison with a build before 6.11:
# the denoiser's and the exposure's history would keep the model's shadow otherwise
$on = if ($Off) { 0 } else { 1 }
$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'skill 1', "r_viewer_model $on")
$end = @('toggleconsole', (Waits 3), 'r_viewer_model 1', 'vid_vsync 1', 'quit')
# the cost: each pass a script of its own (the command buffer holds 8 KB)
$costBlocks = @()
foreach ($pass in 1, 2) {
	$p = @('vk_benchmark 1', 'profiler_samples 120')
	foreach ($i in 0, 1) { $p += @("r_viewer_model $i", (Waits 300), "echo T611_COST $i", 'vk_profiler') }
	$costBlocks += , ($p + 'vk_benchmark 0')
}

$blocks = @()
$lastMap = ''; $lastClass = 0
foreach ($v in $views) {
	$b = @()
	if ($v[1] -ne $lastMap -or $v[2] -ne $lastClass) {
		$b += @("playerclass $($v[2])", "map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip')
		$lastMap = $v[1]; $lastClass = $v[2]
	}
	$b += 'chase_active 0'	# a view's commands may turn it on
	if ($v[3]) { $b += $v[3] }
	$b += @("vk_setpos $($v[4])", (Waits 60))
	if ($v[5]) { $b += $v[5] }
	$b += @('pause', (Waits 10), 'r_viewer_model 0', (Waits 60), "vk_screenshot $($v[0])_v0 8", (Waits 12), "echo T611_LIGHT $($v[0]) 0",
		'r_dumpscene', "r_viewer_model $on", (Waits 60), "vk_screenshot $($v[0])_v1 8", (Waits 12), "echo T611_LIGHT $($v[0]) 1",
		'r_dumpscene', "echo T611_VIEW $($v[0])", 'vk_models', 'vk_models check')
	if ($Cost) { $blocks += , $b; $b = @(); foreach ($p in $costBlocks) { $blocks += , $p } }
	$blocks += , ($b + @('pause', (Waits 5)))
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl611_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$texDir = Join-Path $game 'textures'
if ($Mirror -and (Test-Path $texDir) -and (Get-ChildItem $texDir -Recurse -File)) { throw "data1\textures has files: -Mirror writes its own there" }
$dst = Join-Path $Out $Tag
if (Test-Path $dst) { throw "$dst exists: a fresh -Tag keeps an old run's config backups from being restored" }

# the configs backed up and the folder's shots moved aside before anything is written, and
# everything after inside the try: the finally restores and deletes whatever got done
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
$files = @()
try {
	Get-ChildItem $shots -File | Move-Item -Destination $aside
	for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl611_$i.cfg"; $files += $f; [IO.File]::WriteAllText($f, $texts[$i]) }
	if ($Mirror) {
		New-Item -ItemType Directory -Force $texDir | Out-Null
		# an 8x8 24-bit TGA of 230 grey: the albedo, which tints the mirror
		$px = New-Object byte[] (18 + 8 * 8 * 3); $px[2] = 2; $px[12] = 8; $px[14] = 8; $px[16] = 24
		for ($i = 18; $i -lt $px.Length; $i++) { $px[$i] = 230 }
		foreach ($t in $mirrorTextures) {
			$f = Join-Path $texDir "$t.mat"; $files += $f
			Set-Content -LiteralPath $f -Value @('# viewer_run.ps1 (6.11): a mirror', 'kind chrome', 'roughness 0.01')
			$f = Join-Path $texDir "$t.tga"; $files += $f; [IO.File]::WriteAllBytes($f, $px)
		}
	}
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl611_0.cfg'; Timeout = 2400; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	# the configs and scripts first: a failed move of the shots mustn't keep them
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force } }
	foreach ($f in $files) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -ErrorAction Continue } }
	if ($Mirror -and (Test-Path $texDir) -and -not (Get-ChildItem $texDir -Recurse)) { Remove-Item -LiteralPath $texDir }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation'
# each view's own model (vk_models' viewer triangles) and its check of the instances,
# cl.light_level with both settings; with -Cost its frame and the passes' times
$view = ''
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T611_LIGHT (\S+) (\d)') {
		$name = "$($Matches[1]) r_viewer_model $($Matches[2])"; $lvl = '?'
		for ($j = $i + 1; $j -lt $log.Count -and $log[$j] -notmatch '^T611_'; $j++) {	# r_dumpscene's line, before the next marker
			if ($log[$j] -match '^light level on the weapon \(cl\.light_level, sent to the server\): (\d+)') { $lvl = $Matches[1]; break }
		}
		"${name}: cl.light_level $lvl"
	}
	elseif ($log[$i] -match '^T611_VIEW (\S+)') {
		$view = $Matches[1]; $tris = '?'; $check = '?'
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 30; $j++) {
			if ($log[$j] -match '\+ (\d+) viewer \+') { $tris = $Matches[1] }
			elseif ($log[$j] -match '^models check: .*: (.+)$') { $check = $Matches[1]; break }
		}
		"${view}: $tris viewer triangles, models check: $check"
	}
	elseif ($log[$i] -match '^T611_COST (\d)') {
		$r = $Matches[1]; $frame = ''; $passes = @()
		for ($j = $i + 1; $j -lt $log.Count -and $log[$j] -notmatch '^T611_|^\s+composite and 2D'; $j++) {	# vk_profiler's averages
			if ($log[$j] -match '^frame\s+\S+\s+(\S+)$') { $frame = $Matches[1] }
			elseif ($log[$j] -match '^\s+(dynamic BLASes|TLAS|reflect/refract|direct lighting|bounce 1)\s+\S+\s+(\S+)$') { $passes += "$($Matches[1]) $($Matches[2])" }
		}
		"${view} r_viewer_model ${r}: frame $frame ms; $($passes -join ', ')"
	}
}
