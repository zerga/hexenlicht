# Reflective windows (story 6.20; docs/hexenlicht/TESTING.md, "Windows (6.20)"): the clear
# breakable panes (rtex199) of village1 and village2 from inside lit rooms (the room and the
# player's own model reflected), from the street, at an angle, village1's bay window from the
# front and along it through both of its diagonal panes (a window behind a window). Each view
# goes to the camera (vk_setpos, god, notarget), pauses and shoots for each r_windows value in
# -Windows (default 0,1,2): vk_screenshot <view>_w<n> of 8 frames averaged and <view>_w<n>_1 of
# one (the noise the denoiser leaves); a build before 6.20 (-Bin) doesn't know r_windows: give
# it -Windows 0, its blend. vk_world after each map load prints the panes' triangles. -Cost
# measures each view with vk_benchmark 1, the profiler's averages of 120 paused frames,
# r_windows 0 and 1 alternating twice (Release, e.g. -Width 1920 -Height 1080), instead of the
# shots. -Only a pattern of view names, -Extra a console command after the start. config.cfg
# and hexenlicht.cfg of data1 are backed up and restored, the folder's own shots moved aside
# and back, the scripts deleted. The shots, the log and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [int[]]$Windows = @(0, 1, 2), [string]$Only = '', [string]$Extra = '', [switch]$Cost,
      [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, the camera (x y z pitch yaw: the player's origin, the eye 50 units up)
$views = @(
	@('v1_in_59', 'village1', '-500 1104 50 0 180'),	# inside, the pane *59 against the street
	@('v1_in_66', 'village1', '-1172 2384 50 0 0'),		# inside, *66: the room and the player's own model
	@('v1_in_36', 'village1', '708 1224 50 0 0'),
	@('v1_out_37', 'village1', '1108 1408 50 0 180'),	# the street: *36, *37 with candles inside
	@('v1_out_36', 'village1', '1108 1224 50 0 180'),
	@('v1_out_59', 'village1', '-900 1104 50 0 0'),
	@('v1_angle_37', 'village1', '1000 1180 50 0 112'),	# along the facade
	@('v1_bay_front', 'village1', '511 2224 50 0 90'),	# the bay window *44-*46 from the street
	@('v1_bay_diag', 'village1', '399 2210 50 0 35'),
	@('v1_side_l', 'village1', '330 2352 50 0 0'),		# along the bay: *44 behind *46 on one ray
	@('v1_side_r', 'village1', '700 2352 50 0 180'),
	@('v2_out_14', 'village2', '616 1324 38 0 90'),		# village2's pane *14 from the courtyard
	@('v2_angle_14', 'village2', '480 1400 38 0 30')
)
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'color 0 0', 'playerclass 1')
if ($Extra) { $start += $Extra }
$end = @('toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

# a script per view and cost pass (the command buffer holds 8 KB)
$blocks = @()
$map = ''
foreach ($v in $views) {
	$b = @()
	if ($v[1] -ne $map) { $b += @("map $($v[1])", (Waits 150), 'god', 'notarget', (Waits 20), 'vk_world', (Waits 5)); $map = $v[1] }
	$b += @("vk_setpos $($v[2])", (Waits 30), 'pause', (Waits 20))
	if ($Cost) {
		$blocks += , ($b + @('vk_benchmark 1', 'profiler_samples 120'))
		foreach ($i in 0, 1, 0, 1) { $blocks += , @("r_windows $i", (Waits 300), "echo T620_COST $($v[0]) $i", 'vk_profiler') }
		$blocks += , @('vk_benchmark 0', 'pause', (Waits 5))
	} else {
		foreach ($i in $Windows) {
			$b += @("r_windows $i", (Waits 40), "vk_screenshot $($v[0])_w$i 8", (Waits 20), "vk_screenshot $($v[0])_w${i}_1 1", (Waits 6))
		}
		$blocks += , ($b + @('pause', (Waits 5)))
	}
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl620_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$files = @()
for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl620_$i.cfg"; [IO.File]::WriteAllText($f, $texts[$i]); $files += $f }

$dst = Join-Path $Out $Tag
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
try {
	$a = @{ Cfg = 'hl620_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
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
$log | Select-String '^Vulkan validation|^window panes:'
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T620_COST (\S+) (\d)') {
		$view = $Matches[1]; $r = $Matches[2]; $frame = ''; $rr = ''
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 40; $j++) {
			if ($log[$j] -match '^frame\s+\S+\s+(\S+)$') { $frame = $Matches[1] }
			elseif ($log[$j] -match '^\s+reflect/refract\s+\S+\s+(\S+)$') { $rr = $Matches[1]; break }
		}
		"${view} r_windows ${r}: frame $frame ms, reflect/refract $rr ms"
	}
}
