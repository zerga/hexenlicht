# HDR output (story 7.3; docs/hexenlicht/TESTING.md, "HDR output (7.3)"): at demo1's start
# and meso8's lava room (the lava 4x SDR's white and more), paused, each with:
#  - the albedo view (r_debugview 1, no TAA) shot in SDR and with vid_hdr 1 (equal but for
#    the half-float frame image's rounding: at most 1 of 255 in a few values);
#  - the lit image with vid_hdr 1 (scRGB), 2 (HDR10), r_hdr_decode 1, r_hdr_white 200, FSR
#    (r_scale 67, r_upscaler 2: EASU and RCAS, EASU alone, RCAS alone after TAAU) and the
#    auto exposure (tm_auto_exposure 1), each a vk_screenshot and a vk_hdrshot of the same
#    frame, checked with hdr_check.ps1 against the paper white and peak the game's vk_hdr
#    printed;
# then vk_reload_shaders and vid_restart with HDR on. The display must be in Windows' HDR
# (vk_hdr's first line says why not). -Cost instead measures at demo1's start with
# vk_benchmark 1 the profiler's averages of 120 paused frames, vid_hdr 0 and 1 alternating
# twice (Release, e.g. -Width 2560 -Height 1440; nothing else on the GPU). config.cfg and
# hexenlicht.cfg of data1 are backed up and restored (vid_hdr and r_hdr_* are archived), the
# folder's own shots moved aside and back, the scripts deleted. The shots, the log and the
# results go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [switch]$Cost, [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
$check = Join-Path $PSScriptRoot 'hdr_check.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, the camera (x y z pitch yaw)
$views = @(
	@('demo1', 'demo1', '-918 -2034 0 0 45'),
	@('meso8', 'meso8', '-64 1264 0 0 270')
)
# a pair of vk_screenshot and vk_hdrshot of one frame; clear: no console lines over the shots
function Shots([string]$name) { @('clear', (Waits 2), "vk_screenshot $name; vk_hdrshot $name", (Waits 10)) }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'r_srgb 0', 'flt_fsr_easu 1', 'flt_fsr_rcas 1', 'vid_hdr 0', 'r_hdr_white 0', 'r_hdr_peak 0',
	   'r_hdr_decode 0')
$end = @('vid_hdr 0', 'toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

$blocks = @()
if ($Cost) {
	$v = $views[0]
	$blocks += , @("map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip', (Waits 5), "vk_setpos $($v[2])", (Waits 30), 'pause',
		       'vk_benchmark 1', 'profiler_samples 120')
	foreach ($i in 0, 1, 0, 1) { $blocks += , @("vid_hdr $i", (Waits 300), "echo T73_COST $i", 'vid_hdr', 'vk_profiler') }
	$blocks += , @('vk_benchmark 0', 'pause', (Waits 5))
} else {
	foreach ($v in $views) {
		$n = $v[0]
		$b = @("map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip', (Waits 5), "vk_setpos $($v[2])", (Waits 30), 'pause', (Waits 20),
		       'r_debugview 1', 'r_upscaler 0', (Waits 40), 'clear', (Waits 2), "vk_screenshot ${n}_alb_sdr", (Waits 10),
		       'vid_hdr 1', (Waits 40), 'clear', (Waits 2), "vk_screenshot ${n}_alb_hdr", (Waits 10), 'vk_hdr',
		       'r_debugview 0', 'r_upscaler 1', (Waits 80)) + (Shots "${n}_scrgb")
		$b += @('vid_hdr 2', (Waits 80)) + (Shots "${n}_pq")
		$b += @('vid_hdr 1', 'r_hdr_decode 1', (Waits 80)) + (Shots "${n}_dec1")
		$b += @('r_hdr_decode 0', 'r_hdr_white 200', (Waits 20)) + (Shots "${n}_w200")
		$b += @('r_hdr_white 0', 'r_scale 67', 'r_upscaler 2', (Waits 80)) + (Shots "${n}_fsr")
		$b += @('flt_fsr_rcas 0', (Waits 40)) + (Shots "${n}_easu")
		$b += @('flt_fsr_rcas 1', 'flt_fsr_easu 0', (Waits 40)) + (Shots "${n}_rcas")
		$b += @('flt_fsr_easu 1', 'r_scale 100', 'r_upscaler 1', 'tm_auto_exposure 1', (Waits 120)) + (Shots "${n}_auto")
		$b += @('tm_auto_exposure 0', 'vid_hdr 0', 'pause', (Waits 5))
		$blocks += , $b
	}
	$blocks += , @('vid_hdr 1', (Waits 20), 'vk_reload_shaders', (Waits 20), 'vid_restart', (Waits 60), 'echo T73_AFTER', 'vk_hdr')
}

# all the scripts' sizes checked before any is written
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl73_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$dst = Join-Path $Out $Tag
if (Test-Path $dst) { throw "$dst exists: give another -Tag" }
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
$had = @{}
foreach ($c in 'config.cfg', 'hexenlicht.cfg') {
	$had[$c] = Test-Path (Join-Path $game $c)
	if ($had[$c]) { Copy-Item (Join-Path $game $c) $bk -Force }
}
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
$files = @(for ($i = 0; $i -lt $texts.Count; $i++) { Join-Path $game "hl73_$i.cfg" })
try {
	Get-ChildItem $shots -File | Move-Item -Destination $aside
	for ($i = 0; $i -lt $texts.Count; $i++) { [IO.File]::WriteAllText($files[$i], $texts[$i]) }
	$a = @{ Cfg = 'hl73_0.cfg'; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	# the configs and scripts first: a failed move of the shots mustn't keep them; a config
	# the game wrote that wasn't there before goes
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') {
		if ($had[$c]) { Copy-Item (Join-Path $bk $c) $game -Force }
		elseif (Test-Path (Join-Path $game $c)) { Remove-Item -LiteralPath (Join-Path $game $c) -ErrorAction Continue }
	}
	foreach ($f in $files) { if (Test-Path $f) { Remove-Item -LiteralPath $f -ErrorAction Continue } }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}

$log = Get-Content (Join-Path $dst 'debug_h2.log')
$results = @()
$results += $log | Select-String '^Vulkan validation|^HDR: ' | ForEach-Object { $_.Line }
if ($Cost) {
	for ($i = 0; $i -lt $log.Count; $i++) {
		if ($log[$i] -match '^T73_COST (\d)') {
			$h = $Matches[1]; $frame = ''; $comp = ''; $enc = '-'
			for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 40; $j++) {
				if ($log[$j] -match '^frame\s+\S+\s+(\S+)$') { $frame = $Matches[1] }
				elseif ($log[$j] -match '^\s+composite and 2D\s+\S+\s+(\S+)$') { $comp = $Matches[1] }
				elseif ($log[$j] -match '^\s+HDR output\s+\S+\s+(\S+)$') { $enc = $Matches[1] }
				elseif ($log[$j] -match '^T73_COST') { break }
			}
			$results += "vid_hdr ${h}: frame $frame ms, composite and 2D $comp ms, HDR output $enc ms"
		}
	}
} else {
	# the levels the game used, from vk_hdr
	$lv = $log | Select-String '^paper white (\d+) nits.*, peak (\d+) nits' | Select-Object -First 1
	if (-not $lv) { $results += 'no levels in the log (vk_hdr): no checks' }
	elseif (-not ($log | Select-String '^HDR: on' -Quiet)) { $results += 'HDR never came on (the HDR lines above say why): no checks' }
	else {
		$white = [double]$lv.Matches[0].Groups[1].Value; $peak = [double]$lv.Matches[0].Groups[2].Value
		$results += "levels: paper white $white nits, peak $peak nits"
		function Check([string]$name, [string[]]$more) {
			$t = Join-Path $dst "$name.tga"; $p = Join-Path $dst "$name.pfm"
			if (-not (Test-Path $t) -or -not (Test-Path $p)) { return "${name}: no shot (HDR off then?)" }
			$a = @{ Tga = $t; Pfm = $p; White = $white; Peak = $peak }
			if ($more -contains 'srgb') { $a.Srgb = $true }
			if ($more -contains 'w200') { $a.White = 200 }
			& $check @a
		}
		foreach ($v in $views) {
			$n = $v[0]
			$results += & $check -A (Join-Path $dst "${n}_alb_sdr.tga") -B (Join-Path $dst "${n}_alb_hdr.tga")
			foreach ($k in 'scrgb', 'pq', 'fsr', 'easu', 'rcas', 'auto') { $results += Check "${n}_$k" @() }
			$results += Check "${n}_dec1" @('srgb')
			$results += Check "${n}_w200" @('w200')
		}
	}
}
$results | Set-Content (Join-Path $dst 'results.txt')
$results
