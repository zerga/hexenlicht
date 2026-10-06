# The Renderer Settings page (story 6.10; docs/hexenlicht/TESTING.md, "Settings menu (6.10)"):
# loads demo1, sets the page's settings to their defaults, opens the page (menu_renderer)
# and steps through it with keys posted to the game's window (PostMessage WM_KEYDOWN/UP: no
# focus needed): each step's keys go in after the previous screenshot appears, the script
# shoots every 300 frames. The steps (shot number: keys, what the shot should show):
#   0: -, the defaults (High, 100 %, full, TAAU, Original, fixed, per map (off), off, off,
#      x32); 1: left, Medium 67 %; 2: left, Low 50 % half; 3: left enter, Medium (left stops
#      at Low, enter steps on); 4: down right, 77 %, Custom; 5: down right, bounce two; 6: down
#      right right, DLSS SR (Debug: "DLSS can't run: ..."; Release with the DLLs: its
#      description); 7: right, DLSS RR; 8: enter, TAA (wraps); 9: right down right, TAAU,
#      Physically based (the cursor skips the blank line); 10: down enter, auto exposure;
#      11: down right right, sky light on; 12: down enter down right, both colored lights on;
#      13: down left, x16; 14: down, Reset to defaults; 15: enter, the defaults again;
#      16: down, the cursor wraps to Quality; 17: up, back to Reset; 18: up right right, x64
#      (stops there); 19: escape, the Options menu with "Renderer Settings"; 20: up up enter,
#      the page again; 21: escape down enter, Video Modes; 22: down down right, Vsync yes;
#      23: right, Vsync no; 24: escape up enter, the page again; 25: up x9 down left, the
#      cursor up to Quality and on to Render scale, 77 %, Custom; 26: up left, Medium (Custom
#      steps to the nearest preset by scale); 27: down right x3, 100 % (stays at the end),
#      High; 28: down left left, bounce half (stays), Custom; 29: up up enter, High (from
#      Custom at 100 %); then escape three times out of the menus.
# After the steps the script sets r_maplight_shape 0 in the console and quits; the
# hexenlicht.cfg the game wrote must hold r_emissive_scale "64" and r_maplight_shape "0" and
# none of the page's other settings (saved when changed, vk_menu.c): the script prints the
# page's lines from it. config.cfg and hexenlicht.cfg of data1 are backed up and restored,
# the folder's own shots moved aside and back, the script deleted. The shots, the log, the
# written hexenlicht.cfg and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [string]$Bin = '', [switch]$Release,
      [int]$Width = 1280, [int]$Height = 720, [int]$Timeout = 900, [string]$Data = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$Data = Convert-Path $Data		# absolute: [IO.File] resolves against the process's folder
$game = Join-Path $Data 'data1'
if (-not $Bin) { $Bin = Join-Path $repo "build\$(if ($Release) { 'windows-release' } else { 'windows-debug' })\bin" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class HlMenuKeys {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
"@
# the keys: virtual key, scan code, extended (the arrows are: else MapKey makes them keypad keys)
$keys = @{ U = @(0x26, 0x48, 1); D = @(0x28, 0x50, 1); L = @(0x25, 0x4B, 1); R = @(0x27, 0x4D, 1); E = @(0x0D, 0x1C, 0); X = @(0x1B, 0x01, 0) }
# before each shot (the index), the last after the last shot
$steps = @('', 'L', 'L', 'LE', 'DR', 'DR', 'DRR', 'R', 'E', 'RDR', 'DE', 'DRR', 'DEDR', 'DL', 'D', 'E', 'D', 'U', 'URR', 'X', 'UUE', 'XDE', 'DDR', 'R',
	'XUE', 'UUUUUUUUUDL', 'UL', 'DRRR', 'DLL', 'UUE', 'XXX')
$settings = [ordered]@{ r_scale = '100'; pt_num_bounce_rays = '1'; r_upscaler = '1'; r_maplight_shape = '2'; tm_auto_exposure = '0'
			r_sky_mode = '0'; r_maplight_colors = '0'; gl_colored_dynamic_lights = '0'; r_emissive_scale = '32' }

function Send-Keys([IntPtr]$hwnd, [string]$seq) {
	foreach ($c in $seq.ToCharArray()) {
		$k = $keys["$c"]
		$l = [int64](1 -bor ($k[1] -shl 16) -bor ($k[2] -shl 24))
		[void][HlMenuKeys]::PostMessage($hwnd, 0x100, [IntPtr]$k[0], [IntPtr]$l)
		Start-Sleep -Milliseconds 50
		[void][HlMenuKeys]::PostMessage($hwnd, 0x101, [IntPtr]$k[0], [IntPtr]($l -bor 0xC0000000L))
		Start-Sleep -Milliseconds 100
	}
}

$cfg = @('wait', 'wait', 'wait', 'vid_vsync 0', 'vid_uiscale 3', 'sensitivity 0',
	'alias w10 "wait;wait;wait;wait;wait;wait;wait;wait;wait;wait"', 'alias w100 "w10;w10;w10;w10;w10;w10;w10;w10;w10;w10"',
	'alias shot "w100;w100;w100;screenshot;w10"')
$cfg += $settings.Keys | ForEach-Object { "$_ $($settings[$_])" }
$cfg += @('map demo1', 'w100', 'w100', 'notarget', 'menu_renderer', 'w10')
$cfg += @('shot') * ($steps.Count - 1)
$cfg += @('w100', 'w100', 'w100', 'r_maplight_shape 0', 'w10', 'toggleconsole', 'w10', 'quit')

$dst = Join-Path $Out $Tag
if (Test-Path $dst) { throw "$dst exists: a fresh -Tag keeps an old run's config backups from being restored" }
# the configs backed up and the folder's shots moved aside before anything is written, and
# everything after inside the try: the finally restores and deletes whatever got done
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
$script = Join-Path $game 'hl610_menu.cfg'
try {
	Get-ChildItem $shots -File | Move-Item -Destination $aside
	[IO.File]::WriteAllText($script, ($cfg -join "`r`n") + "`r`n")
	$p = Start-Process (Join-Path $Bin 'hexenlicht.exe') -ArgumentList "-window -width $Width -height $Height -condebug +exec hl610_menu.cfg" -WorkingDirectory $Data -PassThru
	$deadline = (Get-Date).AddSeconds($Timeout)
	while ($p.MainWindowHandle -eq 0 -and -not $p.HasExited) { Start-Sleep -Milliseconds 200; $p.Refresh() }
	for ($i = 1; $i -lt $steps.Count; $i++) {
		$shot = Join-Path $shots ('hexen{0:D2}.tga' -f ($i - 1))
		while (-not (Test-Path $shot) -and -not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
		if ($p.HasExited -or (Get-Date) -ge $deadline) { "stopped before step $i"; break }
		Start-Sleep -Milliseconds 400
		Send-Keys $p.MainWindowHandle $steps[$i]
	}
	if (-not $p.WaitForExit([Math]::Max(1000, ($deadline - (Get-Date)).TotalMilliseconds))) { 'TIMEOUT'; $p.Kill() } else { "exit $($p.ExitCode)" }
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
	if (Test-Path (Join-Path $game 'hexenlicht.cfg')) { Copy-Item (Join-Path $game 'hexenlicht.cfg') (Join-Path $dst 'written.cfg') -Force }
} finally {
	# the configs and the script first: a failed move of the shots mustn't keep them
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') {
		if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force }
		elseif ($c -eq 'hexenlicht.cfg') { [IO.File]::Delete((Join-Path $game $c)) }	# the run's own
	}
	if (Test-Path -LiteralPath $script) { Remove-Item -LiteralPath $script -ErrorAction Continue }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
Get-Content (Join-Path $dst 'debug_h2.log') | Select-String '^Vulkan validation'
"$((Get-ChildItem $dst -Filter 'hexen*.tga').Count) shots of $($steps.Count - 1)"
'saved (expected: r_emissive_scale "64", r_maplight_shape "0", gl_colored_dynamic_lights "0" (always saved)):'
Get-Content (Join-Path $dst 'written.cfg') | Where-Object { $_ -match '^(\S+) ' -and $settings.Contains($Matches[1]) }
