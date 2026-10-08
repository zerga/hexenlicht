# Smoother model animation (story 8.2; docs/hexenlicht/TESTING.md, "Model smoothing
# (8.1, 8.2)"): each view loads its map, creates a monster with the "create" cheat (80 units
# ahead of the last frame's view) or shows the player's own model in the chase camera, lets it
# idle, pauses and shoots the same paused frame three times (vk_screenshot, 8 frames
# averaged): <view>_orig (r_smoothmodels 0: the bytes and the table normals), <view>_side
# (smoothed, r_smoothseams 0: each seam side its own normal) and <view>_weld (smoothed,
# r_smoothseams 1, the default), with vk_models check after each. Data1: an ice imp and a
# bronze golem at village1's start (daylight), the Paladin in the chase camera; -Portals: Portal of Praevus'
# imp and golem (its welded models) at keep1's start. -Only a pattern of view names.
# config.cfg and hexenlicht.cfg of the game folder are backed up and restored, its shots
# moved aside and back, the scripts deleted. The shots, the log and the validation line go
# to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [switch]$Portals, [string]$Only = '', [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data $(if ($Portals) { 'portals' } else { 'data1' })
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, player class, the setup (with noclip on), the camera (x y z pitch yaw, '' = stay)
$street = 'vk_setpos 200 -784 30 0 90'	# village1's start, in daylight
if ($Portals) {
	$views = @(
		@('k1_imp', 'keep1', 2, @('vk_setpos 1400 1256 -208 0 180', (Waits 20), 'create monster_imp_ice', (Waits 120)), '1400 1256 -196 5 180'),
		@('k1_golem', 'keep1', 2, @('vk_setpos 1400 1256 -208 0 180', (Waits 20), 'create monster_golem_bronze', (Waits 120)), '1540 1256 -170 8 180')
	)
} else {
	$views = @(
		@('v1_imp', 'village1', 2, @($street, (Waits 20), 'create monster_imp_ice', (Waits 120)), '200 -610 40 5 270'),
		@('v1_golem', 'village1', 2, @($street, (Waits 20), 'create monster_golem_bronze', (Waits 120)), '200 -520 60 10 270'),
		@('v1_paladin', 'village1', 1, @('vk_setpos 200 -560 30 0 90', 'chase_active 1', 'chase_back 60', 'chase_up 16', (Waits 120)), '')
	)
}
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'r_drawviewmodel 0', 'skill 1')
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')
$prefix = 'hl82'

$blocks = @()
foreach ($v in $views) {
	$b = @("playerclass $($v[2])", "map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip', 'r_smoothmodels 1', 'r_smoothseams 1') + $v[3]
	if ($v[4]) { $b += @("vk_setpos $($v[4])", (Waits 30)) }
	$b += @('pause', (Waits 10))
	foreach ($s in @(@('orig', 'r_smoothmodels 0'), @('side', 'r_smoothmodels 1;r_smoothseams 0'), @('weld', 'r_smoothseams 1'))) {
		$b += @($s[1], (Waits 20), "vk_screenshot $($v[0])_$($s[0]) 8", (Waits 12), "echo T82_VIEW $($v[0])_$($s[0])", 'vk_models check')
	}
	$blocks += , ($b + @('pause', 'chase_active 0', (Waits 5)))
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec ${prefix}_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$files = @()
for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "${prefix}_$i.cfg"; [IO.File]::WriteAllText($f, $texts[$i]); $files += $f }

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
try {
	$a = @{ Exe = 'hexenlicht'; Cfg = "${prefix}_0.cfg"; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	if ($Portals) { $a.Portals = $true }
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
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T82_VIEW (\S+)') {
		$view = $Matches[1]; $check = '?'
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 12; $j++) {
			if ($log[$j] -match '^models check: .*: (.+)$') { $check = $Matches[1]; break }
		}
		"${view}: models check: $check"
	}
}
