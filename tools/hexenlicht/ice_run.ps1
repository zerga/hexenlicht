# Frozen monsters as ice (story 6.15; docs/hexenlicht/TESTING.md, "Ice (6.15)"): each view
# loads its map, freezes a monster with vk_freeze (the gamecode's own SnowJob, kept frozen; a
# monster the map has is spawned with the "create" cheat where none stands in the view), waits
# out the freeze's 1.5 s tint (the ice skin after it), goes to the camera, pauses and shoots
# twice, with r_ice 0 (6.4's blend) and 1 (vk_screenshot <view>_i0 and _i1, 8 frames
# averaged): demo1's archer from the front and from the side, one created in village1's bay
# window seen from the street through the panes, one created at demo1's pool, village2's
# crystal golem (ice from the start). -Saves also saves each view (hlcal_ice_<view>, for
# calib_shots.ps1 -SkipSaves -SkipHl -KeepSaves with the bookmarks file it writes into -Out:
# glh2's shots of the same scenes; GL draws the 0.33 blend); -Cost measures each view with
# vk_benchmark 1, the profiler's averages of 120 paused frames, r_ice 0 and 1 alternating twice
# (Release, e.g. -Width 1920 -Height 1080). -Only a pattern of view names. config.cfg and
# hexenlicht.cfg of data1 are backed up and restored, the folder's own shots moved aside and
# back, the scripts deleted. The shots, the log and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [string]$Only = '', [switch]$Saves, [switch]$Cost, [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, the freeze (with noclip on), the camera (x y z pitch yaw)
$freeze = @((Waits 20), 'vk_freeze', (Waits 100))
$create = @((Waits 20), 'create monster_archer', (Waits 60), 'vk_freeze', (Waits 100))	# create: 80 units ahead of the last frame's view
$views = @(
	@('d1_archer', 'demo1', (@('vk_setpos -1640 -690 -136 0 90') + $freeze), '-1640 -630 -136 0 90'),
	@('d1_archer_side', 'demo1', (@('vk_setpos -1640 -690 -136 0 90') + $freeze), '-1730 -530 -136 0 -20'),
	@('v1_bay', 'village1', (@('vk_setpos 511 2345 10 0 90') + $create), '511 2224 50 10 90'),
	@('d1_pool', 'demo1', (@('vk_setpos -1060 2300 -522 0 0') + $create), '-1080 2300 -522 50 0'),
	@('v2_golem', 'village2', @(), '-1116 -640 -600 0 90')
)
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'r_drawviewmodel 0', 'playerclass 2', 'skill 1')
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')
# the cost: each pass a script of its own (the command buffer holds 8 KB)
$costBlocks = @()
foreach ($pass in 1, 2) {
	$p = @('vk_benchmark 1', 'profiler_samples 120')
	foreach ($i in 0, 1) { $p += @("r_ice $i", (Waits 300), "echo T615_COST $i", 'vk_profiler') }
	$costBlocks += , ($p + 'vk_benchmark 0')
}

$blocks = @()
foreach ($v in $views) {
	# a save on vk_setpos's line keeps the pitch (TESTING.md, calib_shots.ps1)
	$b = @("map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip', 'r_ice 1') + $v[2] +
	     @("vk_setpos $($v[3])$(if ($Saves) { "; save hlcal_ice_$($v[0])" })", (Waits 30))
	$b += @('pause', (Waits 10), 'r_ice 0', (Waits 60), "vk_screenshot $($v[0])_i0 8", (Waits 12),
		'r_ice 1', (Waits 60), "vk_screenshot $($v[0])_i1 8", (Waits 12), "echo T615_VIEW $($v[0])", 'vk_models', 'vk_models check')
	if ($Cost) { $blocks += , $b; $b = @(); foreach ($p in $costBlocks) { $blocks += , $p } }
	$blocks += , ($b + @('r_ice 1', 'pause', (Waits 5)))
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl615_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$files = @()
for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl615_$i.cfg"; [IO.File]::WriteAllText($f, $texts[$i]); $files += $f }

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
try {
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl615_0.cfg'; Timeout = 1800; Width = $Width; Height = $Height; Data = $Data }
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
if ($Saves) {
	# calib_shots.ps1's bookmark lines: only the names and maps count with -SkipSaves
	$views | ForEach-Object { "ice_$($_[0]) $($_[1]) 0 0 0 0 0" } | Set-Content (Join-Path $Out 'ice_bookmarks.txt')
}
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation|^vk_freeze'
# each view's ice (vk_models' last line) and its check of the instances, with -Cost its frame
# and reflect/refract times
$view = ''
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T615_VIEW (\S+)') {
		$view = $Matches[1]; $ice = '?'; $check = '?'
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 30; $j++) {
			if ($log[$j] -match '(\d+) of ice \(6\.15\)') { $ice = $Matches[1] }
			elseif ($log[$j] -match '^models check: .*: (.+)$') { $check = $Matches[1]; break }
		}
		"${view}: $ice instances of ice, models check: $check"
	}
	elseif ($log[$i] -match '^T615_COST (\d)') {
		$r = $Matches[1]; $frame = ''; $rr = ''
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 30; $j++) {
			if ($log[$j] -match '^frame\s+\S+\s+(\S+)$') { $frame = $Matches[1] }
			elseif ($log[$j] -match '^\s+reflect/refract\s+\S+\s+(\S+)$') { $rr = $Matches[1]; break }
		}
		"${view} r_ice ${r}: frame $frame ms, reflect/refract $rr ms"
	}
}
