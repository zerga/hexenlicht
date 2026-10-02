# Water views (story 6.5; docs/hexenlicht/TESTING.md, "Water (6.5)"): above and below the
# translucent water (*lowlight: demo2's deep pool; *rtex078: demo1's and romeric3's pools), the
# opaque water (castle4's *rtex153, demo3's *rtex346, meso8's *skulls, the cathedral's shallow
# *lowlight), across pools at grazing angles (romeric3, romeric1, village5), the view turning
# above and below the water, lava from above and below (meso8), and the crossbow's bolts under
# water (their bubbles).
#  -Saves: a Hexenlicht run that saves each view (hl65_<view> in data1: noclip, so the player
#   neither falls nor sinks, vk_setpos, then save; every weapon, mana and item: impulse 43;
#   god, notarget).
#  Else one engine (-Exe hexenlicht or glh2; -Bin another build's folder) loads each save:
#   a view pauses and shoots (Hexenlicht: vk_screenshot of 8 frames averaged and of 1;
#   glh2: screenshot, renamed); a weapon step fires and shoots with the game running
#   (host_framerate 0.02: two runs from the same saves show the same moments). The shots, the
#   log and the validation line go to -Out\<Tag>.
# -Only a pattern of view names, -Extra a console command after the start (e.g.
# 'r_water 0'). config.cfg and hexenlicht.cfg of data1 are backed up and restored,
# the folder's own shots moved aside and back, the scripts deleted (the saves are kept for the
# next runs: delete the hl65_* folders in data1 when done).
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'saves', [ValidateSet('hexenlicht', 'glh2')][string]$Exe = 'hexenlicht',
      [string]$Bin = '', [switch]$Release, [switch]$Saves, [string]$Only = '', [string]$Extra = '',
      [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
$gl = $Exe -eq 'glh2'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, class, position (x y z pitch yaw), what happens after the load, Hexenlicht's commands after the shots.
# A step: '' a paused view; 'turn f,f,...' the view turning right at 20 degrees a second, a shot at each frame;
# 'i[+i] burst f,f,...' weapon impulses, +attack for burst frames, shots at those frames after it began;
# 'i[+i] burst pause' fired, paused after burst frames, shot as a view
$views = @(
	@('d2_above', 'demo2', 1, '-468 -670 -58 30 0', '', ''),
	@('d2_above_b', 'demo2', 1, '-468 -670 -58 30 180', '', ''),
	@('d2_under', 'demo2', 1, '-468 -670 -218 -25 0', '', ''),
	@('d2_under_lvl', 'demo2', 1, '-468 -670 -218 0 0', '', ''),
	@('d1_078_above', 'demo1', 1, '-1060 2300 -522 30 0', '', ''),
	@('d1_078_under', 'demo1', 1, '-1060 2300 -682 -25 0', '', ''),
	@('r3_above', 'romeric3', 1, '460 880 -226 30 0', '', ''),
	@('r3_under', 'romeric3', 1, '460 880 -380 -25 0', '', ''),
	@('c4_above', 'castle4', 1, '1258 411 -170 30 0', '', ''),
	@('d3_above', 'demo3', 1, '3715 73 238 30 0', '', ''),
	@('d3_under', 'demo3', 1, '3715 73 150 0 0', '', ''),
	@('cath_above', 'cath', 1, '1795 1199 -748 30 0', '', ''),
	@('m8_above', 'meso8', 1, '480 3600 22 30 0', '', ''),
	@('r3_across', 'romeric3', 1, '200 880 -226 8 0', '', ''),
	@('r3_across_turn', 'romeric3', 1, '200 880 -226 8 0', 'turn 20,40,60', ''),
	@('d1_078_under_turn', 'demo1', 1, '-1060 2300 -682 -25 0', 'turn 20,40,60', ''),
	@('r1_across', 'romeric1', 1, '22 -70 0 10 0', '', ''),
	@('v5_across', 'village5', 1, '-460 1861 -440 10 0', '', ''),
	@('m8_lava_above', 'meso8', 1, '255 1099 40 30 0', '', ''),
	@('m8_lava_under', 'meso8', 1, '255 1099 -82 -20 0', '', ''),
	@('d1_078_bubbles', 'demo1', 4, '-1060 2300 -682 0 0', '2 10 10,20,35', '')
)
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$names = New-Object System.Collections.Generic.List[string]
function Shot([string]$name) {
	$names.Add($name)
	if ($gl) { return @('screenshot', (Waits 12)) }
	$names.Add("${name}_1")
	return @("vk_screenshot $name 8", (Waits 12), "vk_screenshot ${name}_1 1", (Waits 6))
}
function Frame([string]$name) { $names.Add($name); if ($gl) { 'screenshot' } else { "vk_screenshot $name 1" } }
$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'viewsize 130', 'showpause 0', 'crosshair 0', 'con_notifytime 0', 'color 0 0')
if ($Extra) { $start += $Extra }
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

$blocks = @()
if ($Saves) {
	foreach ($g in ($views | Group-Object { "$($_[1]) $($_[2])" })) {
		$v0 = $g.Group[0]
		$b = @("playerclass $($v0[2])", "map $($v0[1])", (Waits 150), 'god', 'notarget', 'impulse 43', 'noclip', (Waits 30))
		foreach ($v in $g.Group) { $b += @("vk_setpos $($v[3])", "save hl65_$($v[0])", (Waits 5)) }
		$blocks += , $b
	}
} else {
	foreach ($v in $views) {
		if (-not (Test-Path (Join-Path $game "hl65_$($v[0])"))) { throw "no save hl65_$($v[0]): run with -Saves first" }
		$b = @("load hl65_$($v[0])", (Waits 20))
		$step = $v[4]
		if ($step -match '^turn ') {
			$b += @('cl_yawspeed 20', '+right'); $t = 0
			foreach ($d in (($step -split ' ')[1] -split ',' | ForEach-Object { [int]$_ })) {
				$b += @((Waits ($d - $t)), (Frame "$($v[0])_$d")); $t = $d
			}
			$b += @('-right', 'cl_yawspeed 140', (Waits 12))	# the last frame's shot before the next load
		} elseif ($step) {
			$imp, $burst, $at = $step -split ' '
			foreach ($i in $imp -split '\+') { $b += @("impulse $i", (Waits 100)) }	# a weapon change takes up to 100 frames
			if ($at -eq 'pause') {
				$b += @('+attack', (Waits ([int]$burst)), 'pause', '-attack', (Waits 40)) + (Shot $v[0]) + @('pause')
			} else {
				$b += '+attack'; $t = 0; $released = $false
				foreach ($d in ($at -split ',' | ForEach-Object { [int]$_ })) {
					if (-not $released -and [int]$burst -le $d) { $b += @((Waits ([int]$burst - $t)), '-attack'); $t = [int]$burst; $released = $true }
					$b += @((Waits ($d - $t)), (Frame "$($v[0])_$d")); $t = $d
				}
				if (-not $released) { $b += '-attack' }
			}
		} else {
			$b += @('pause', (Waits 40)) + (Shot $v[0])
			if (-not $gl -and $v[5]) { $b += @("echo T65_PROBE $($v[0])", $v[5], (Waits 5)) }
			$b += 'pause'
		}
		$blocks += , $b
	}
}

$files = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl65_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$f = Join-Path $game "hl65_$i.cfg"; [IO.File]::WriteAllText($f, $text); $files += $f
}

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
try {
	$a = @{ Exe = $Exe; Cfg = 'hl65_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	if ($gl) {
		$t = @(Get-ChildItem $shots -Filter 'hexen*.tga' | Sort-Object Name)
		for ($i = 0; $i -lt $t.Count -and $i -lt $names.Count; $i++) { Move-Item $t[$i].FullName (Join-Path $dst "$($names[$i]).tga") -Force }
		"$($t.Count) shots for $($names.Count) names"
	}
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
$i = 0
while ($i -lt $log.Count) {
	if ($log[$i] -match '^T65_PROBE (\S+)') {
		$view = $Matches[1]; $hits = @(); $j = $i + 2
		while ($j -lt $log.Count -and $log[$j] -match '^\s+t\s') { if ($log[$j] -match ': (\*\d+|world)') { $hits += $Matches[1] }; $j++ }
		"probe ${view}: " + (($hits | Select-Object -First 6) -join ', ')
		$i = $j
	} else { $i++ }
}
