# Measures the renderer's performance baseline with the GPU profiler
# (vk_profiler.c): for each window size, runs hexenlicht.exe once with a
# generated test script that visits demo1's and the cathedral's starts with
# the maps' lights (4.1; 3.11's baseline had test lights at the light
# entities, vk_testlight entities, 1000 each and none dropped inside
# solid), paused, with vk_benchmark 1 (no 72 fps cap, no sleeps), viewsize 100 and
# fov 90, and for each setting prints vk_profiler's averages over -Samples
# frames. The settings: TAAU at 100 % and 67 %, and DLSS RR at 67 % when
# sl.interposer.dll is next to the exe (a column whose upscaler row isn't
# "DLSS RR" is marked: DLSS didn't run). Prints markdown tables of the
# averages (ms per pass), with the GPU, the driver and its power limit
# (nvidia-smi), and writes them to -Out when given. The Release build unless
# -DebugBuild. Backs up data1\config.cfg and data1\hexenlicht.cfg and puts
# them back (if that fails, it says where the backup is); deletes the
# scripts it wrote. The window sizes must fit the desktop. See
# docs/hexenlicht/TESTING.md ("GPU cost").
param([string[]]$Sizes = @('1920x1080', '2560x1440'), [switch]$DebugBuild, [ValidateRange(10, 600)][int]$Samples = 120,
      [string]$Data = '', [string]$Bin = '', [string]$Out = '', [switch]$NoBenchmark, [int]$Timeout = 600)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$Data = Convert-Path $Data		# absolute: [IO.File] resolves against the process's folder
$preset = if ($DebugBuild) { 'windows-debug' } else { 'windows-release' }
if (-not $Bin) { $Bin = Join-Path $repo "build\$preset\bin" }
$Bin = Convert-Path $Bin
$data1 = Join-Path $Data 'data1'
$log = Join-Path $Data 'debug_h2.log'

$scenes = @('demo1', 'cath')
$settings = [ordered]@{ 'TAAU 100 %' = 'r_upscaler 1; r_scale 100'; 'TAAU 67 %' = 'r_upscaler 1; r_scale 67' }
if (Test-Path (Join-Path $Bin 'sl.interposer.dll')) { $settings['DLSS RR 67 %'] = 'r_upscaler 4; r_scale 67' }

function Waits([int]$n) { (1..$n | ForEach-Object { 'wait' }) -join "`r`n" }

# the owner's settings stay as they were
$backup = Join-Path ([IO.Path]::GetTempPath()) ('hl_perf_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $data1 $c)) { Copy-Item (Join-Path $data1 $c) $backup } }

$gpu = ''
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
	$gpu = (nvidia-smi --query-gpu=name,driver_version,power.limit --format=csv,noheader) -join '; '
}
$report = @("Performance baseline, $(Get-Date -Format 'yyyy-MM-dd'); $(if ($NoBenchmark) { 'vk_benchmark 0' } else { 'vk_benchmark 1' }), $preset, averages over $Samples frames (ms)")
if ($gpu) { $report += "GPU (nvidia-smi: name, driver, power limit): $gpu" }

$files = @()
try {
	# the test scripts: one per scene and setting, each exec'ing the next (a
	# script over ~8 KB overflows the command buffer)
	$first = @(
		(Waits 3), 'vid_vsync 0', 'con_notifytime 0', 'showpause 0', 'playerclass 1', 'host_framerate 0.02',
		'viewsize 100', 'fov 90', 'r_debugview 0', "profiler_samples $Samples",
		"vk_benchmark $(if ($NoBenchmark) { 0 } else { 1 })", 'exec hl_perf_0.cfg')
	$files += 'hl_perf.cfg'
	Set-Content -Path (Join-Path $data1 'hl_perf.cfg') -Value ($first -join "`r`n")
	$n = 0
	foreach ($scene in $scenes) {
		$lines = @('pause', "map $scene", (Waits 150), 'notarget', 'god', (Waits 330), 'pause', (Waits 10))
		foreach ($s in $settings.Keys) {
			$lines += @($settings[$s], (Waits ([Math]::Max(300, $Samples + 100))), "echo ==== PERF $scene $s", 'vk_profiler', 'vk_upscale')
			$lines += "exec hl_perf_$($n + 1).cfg"
			$files += "hl_perf_$n.cfg"
			Set-Content -Path (Join-Path $data1 "hl_perf_$n.cfg") -Value ($lines -join "`r`n")
			$n++
			$lines = @()
		}
	}
	$files += "hl_perf_$n.cfg"
	Set-Content -Path (Join-Path $data1 "hl_perf_$n.cfg") -Value (@('vk_benchmark 0', 'vid_vsync 1', 'toggleconsole', (Waits 5), 'quit') -join "`r`n")

	foreach ($size in $Sizes) {
		$w, $h = $size.Split('x') | ForEach-Object { [int]$_ }
		# defaults: no hexenlicht.cfg, whose archived cvars (the owner's or an
		# earlier run's) would change what is measured; config.cfg as it was
		[IO.File]::Delete((Join-Path $data1 'hexenlicht.cfg'))
		if (Test-Path (Join-Path $backup 'config.cfg')) { Copy-Item (Join-Path $backup 'config.cfg') $data1 -Force }
		$runArgs = @{ Cfg = 'hl_perf.cfg'; Width = $w; Height = $h; Timeout = $Timeout; Data = $Data; Bin = $Bin }
		$result = & (Join-Path $PSScriptRoot 'hl_run.ps1') @runArgs
		$text = Get-Content $log
		$device = ($text | Select-String -Pattern '^Vulkan: using (.*) \(' | Select-Object -First 1).Matches.Groups[1].Value
		# the tables: the rows after each "==== PERF" marker's header
		$columns = [ordered]@{}
		$rows = New-Object System.Collections.Generic.List[string]
		for ($i = 0; $i -lt $text.Count; $i++) {
			if ($text[$i] -notmatch '^==== PERF (\S+) (.+?)\s*$') { continue }
			$col = "$($matches[1]) $($matches[2])"
			$values = @{}
			$prev = $null
			for ($j = $i + 2; $j -lt $text.Count -and $text[$j] -match '^(\s*)(\S.*?)\s+(\d+\.\d+)\s+(\d+\.\d+)$'; $j++) {
				$name = ('&nbsp;' * (2 * $matches[1].Length)) + $matches[2]
				# a row first seen here goes after the row above it in this table
				if (-not $rows.Contains($name)) { $rows.Insert($(if ($prev) { $rows.IndexOf($prev) + 1 } else { 0 }), $name) }
				$values[$name] = $matches[4]
				$prev = $name
			}
			if ($col -match 'DLSS' -and -not ($values.Keys | Where-Object { $_ -match 'DLSS' })) { $col += ' (DLSS not running)' }
			$columns[$col] = $values
		}
		$report += ''
		$report += "### ${size}: $device ($result)"
		$report += ''
		$report += '| Pass | ' + (($columns.Keys) -join ' | ') + ' |'
		$report += '|---' * ($columns.Count + 1) + '|'
		foreach ($r in $rows) {
			$report += "| $r | " + (($columns.Keys | ForEach-Object { if ($columns[$_].ContainsKey($r)) { $columns[$_][$r] } else { '' } }) -join ' | ') + ' |'
		}
	}
}
finally {
	$restored = $true
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') {
		$src = Join-Path $backup $c
		try {
			if (Test-Path $src) { Copy-Item $src $data1 -Force } elseif ($c -eq 'hexenlicht.cfg') { [IO.File]::Delete((Join-Path $data1 $c)) }
		}
		catch { $restored = $false; Write-Warning "could not restore $c ($_)" }
	}
	foreach ($f in $files) { [IO.File]::Delete((Join-Path $data1 $f)) }
	if ($restored) {
		foreach ($c in 'config.cfg', 'hexenlicht.cfg') { [IO.File]::Delete((Join-Path $backup $c)) }
		[IO.Directory]::Delete($backup)
	}
	else { Write-Warning "the configs' backup is in $backup" }
}
$report
if ($Out) { Set-Content -Path $Out -Value ($report -join "`r`n") }
