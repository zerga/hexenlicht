# The medium's light along a swim through demo2's moat (story 6.17; docs/hexenlicht/TESTING.md,
# "Water (6.5)"): the player is moved along a path through the moat at -Speed units a second
# (vk_setpos every frame at host_framerate 0.02, noclip, looking along the path), from the
# moat's south-west end east, north, over the submerged wall into the east moat and north
# along it to its end. 6.18: -Lift raises the path (230: the eye 100-160 units above the
# water, looking into the moat from the air) and -Pitch tilts the view down.
#  Without -Shots: r_drawentities 0 (shorter dumps; the light doesn't depend on them) and an
#   r_dumpscene every -Every frames; the camera, cl.light_level (/ 200: the medium's light
#   in the air before 6.18, under water before 6.17) and the medium light line (6.17: the
#   eased light, 6.18: the light grid's at the eye) go to -Out\<Tag>\medium.csv, with the
#   largest change of each within -Window seconds under water, and of cl.light_level in the
#   air.
#  With -Shots f,f,...: those frames shot (vk_screenshot, 1 frame, the game running) as
#   swim_<f>; -Bin another build (e.g. main's, for the look before).
# config.cfg and hexenlicht.cfg of data1 are backed up and restored, the folder's own shots
# moved aside and back, the scripts deleted.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'swim', [string]$Bin = '', [switch]$Release,
      [string]$Shots = '', [int]$Every = 2, [double]$Window = 0.2, [double]$Speed = 200, [string]$Extra = '',
      [double]$Lift = 0, [double]$Pitch = 0, [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
$inv = [Globalization.CultureInfo]::InvariantCulture
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# the path (x y z of the player's origin; the eye is 50 units higher): z -280 (the water is
# -100 to -330; the east moat's tunnel, x 870-1250, from -200), over the wall between the
# south and the east moat at -170 (its top at about -160, the water's surface at -50 there)
$path = @(@(-1024, -852, -280), @(256, -852, -280), @(256, -284, -280), @(380, -270, -170),
          @(620, -270, -170), @(720, -284, -280), @(1500, -284, -280), @(1500, 1760, -280))
$step = $Speed * 0.02
$frames = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $path.Count - 1; $i++) {
	$a = $path[$i]; $b = $path[$i + 1]
	$d = @(($b[0] - $a[0]), ($b[1] - $a[1]), ($b[2] - $a[2]))
	$len = [Math]::Sqrt($d[0] * $d[0] + $d[1] * $d[1] + $d[2] * $d[2])
	$yaw = [Math]::Round([Math]::Atan2($d[1], $d[0]) * 180 / [Math]::PI)
	$n = [Math]::Max(1, [int][Math]::Round($len / $step))
	for ($k = 0; $k -lt $n; $k++) {
		$t = $k / $n
		$frames.Add([string]::Format($inv, 'vk_setpos {0:F0} {1:F0} {2:F0} {3:F0} {4}', ($a[0] + $d[0] * $t), ($a[1] + $d[1] * $t), ($a[2] + $d[2] * $t + $Lift), $Pitch, $yaw))
	}
}
$shotAt = @{}
if ($Shots) { foreach ($f in $Shots -split ',') { $shotAt[[int]$f] = $true } }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0', 'con_notifytime 0', 'color 0 0',
           'playerclass 1', 'map demo2', (Waits 150), 'god', 'notarget', 'noclip', (Waits 30))
if (-not $Shots) { $start += 'r_drawentities 0' }
if ($Extra) { $start += $Extra }
# 20 frames at the start of the path first: the camera comes into the water there. A shot
# goes on its frame's line (vk_screenshot takes the frame drawn after the commands); a dump
# after the wait (r_dumpscene prints the frame drawn before)
$body = @("$($frames[0])", (Waits 20))
for ($f = 0; $f -lt $frames.Count; $f++) {
	$line = "$($frames[$f])"
	if ($Shots) { if ($shotAt[$f]) { $line += ";vk_screenshot swim_$f 1" }; $line += ';wait' }
	elseif ($f % $Every -eq 0) { $line += ';wait;r_dumpscene' }
	else { $line += ';wait' }
	$body += $line
}
$end = @('r_drawentities 1', 'toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

$files = @(); $chunk = @(); $chunks = @()
foreach ($l in $start + $body) {
	if ($l -eq '') { continue }
	$chunk += $l
	if ((($chunk -join "`r`n").Length) -gt 7000) { $chunks += , $chunk; $chunk = @() }
}
$chunks += , ($chunk + $end)
for ($i = 0; $i -lt $chunks.Count; $i++) {
	$lines = $chunks[$i]
	if ($i -lt $chunks.Count - 1) { $lines += "exec hl617_$($i + 1).cfg" }
	$text = ($lines -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$f = Join-Path $game "hl617_$i.cfg"; [IO.File]::WriteAllText($f, $text); $files += $f
}
"$($frames.Count) frames on the path, $($chunks.Count) scripts"

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
Get-ChildItem $bk -File | Remove-Item	# an earlier run's: restored below only if data1 has it now
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shotDir = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shotDir | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shotDir -File | Move-Item -Destination $aside -Force
try {
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl617_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	Get-ChildItem $shotDir -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force } }
	foreach ($f in $files) { Remove-Item -LiteralPath $f -ErrorAction Continue }
	Get-ChildItem $aside -File | Move-Item -Destination $shotDir -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation'
if ($Shots) { return }

# the dumps: camera, cl.light_level, the medium light line (6.17's or 6.18's)
$rows = @('frame,time,x,y,z,leaf,light_level_200,now,medium,points'); $cam = $null; $leaf = ''; $time = '0'; $lvl = 0
$v = New-Object System.Collections.Generic.List[object]	# per dump: old, now, medium, under water, in the air (not in the ground)
foreach ($l in $log) {
	if ($l -match '^Scene of frame \d+, time ([\d.]+)') { $time = $Matches[1] }
	elseif ($l -match '^camera: org (\S+) (\S+) (\S+)') { $cam = @($Matches[1], $Matches[2], $Matches[3]) }
	elseif ($l -match 'leaf \d+ \((\w+)\)') { $leaf = $Matches[1] }
	elseif ($l -match '^light level on the weapon .*: (\d+)') { $lvl = [int]$Matches[1] }
	elseif ($l -match '^medium light') {
		# 6.17: eased and this frame's under water, cl.light_level's in the air; 6.18: the
		# light grid's at the eye, without and with the dynamic lights (-1: outside it)
		$old = $lvl / 200.0; $now = $old; $med = $old; $pts = 0
		if ($l -match ': ([\d.]+) eased, ([\d.]+) this frame over (\d+) points') { $med = [double]$Matches[1]; $now = [double]$Matches[2]; $pts = [int]$Matches[3] }
		elseif ($l -match ': ([\d.]+), cl\.light_level') { $med = [double]$Matches[1]; $now = $med }
		elseif ($l -match 'light grid, / 200\): ([\d.]+), with the dynamic lights ([\d.]+)') { $now = [double]$Matches[1]; $med = [double]$Matches[2] }
		elseif ($l -match 'outside the liquids') { $now = -1; $med = -1 }
		$rows += [string]::Format($inv, '{0},{1},{2},{3},{4},{5},{6:F3},{7:F3},{8:F3},{9}', ($v.Count * $Every), $time, $cam[0], $cam[1], $cam[2], $leaf, $old, $now, $med, $pts)
		$v.Add(@($old, $now, $med, ($leaf -eq 'water'), ($leaf -eq 'empty')))
	}
}
$rows | Set-Content (Join-Path $dst 'medium.csv')
# the largest change of each within the window, over stretches under water; of cl.light_level
# (the medium's light in the air before 6.18) over stretches in the air
$w = [Math]::Max(1, [int][Math]::Round($Window / 0.02 / $Every)); $max = @(0.0, 0.0, 0.0); $air = 0.0; $streak = 0; $astreak = 0
for ($i = 0; $i -lt $v.Count; $i++) {
	if ($v[$i][3]) { $streak++ } else { $streak = 0 }
	if ($v[$i][4]) { $astreak++ } else { $astreak = 0 }
	if ($astreak -gt $w) { $air = [Math]::Max($air, [Math]::Abs($v[$i][0] - $v[$i - $w][0])) }
	if ($streak -le $w) { continue }
	for ($k = 0; $k -lt 3; $k++) { $max[$k] = [Math]::Max($max[$k], [Math]::Abs($v[$i][$k] - $v[$i - $w][$k])) }
}
[string]::Format($inv, '{0} dumps; the largest change within {1} s ({2} frames) under water: cl.light_level / 200 {3:F3}, the medium light line''s first {4:F3}, its second {5:F3}; in the air: cl.light_level / 200 {6:F3}',
                 $v.Count, $Window, ($w * $Every), $max[0], $max[1], $max[2], $air)
