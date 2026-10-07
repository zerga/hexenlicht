# Portal of Praevus views (story 6.8; docs/hexenlicht/TESTING.md, "Praevus (6.8)"): the flames
# of the mission pack's light entities, from saves so that both engines and any build show the
# same camera. -Saves makes the saves (Hexenlicht: map, god, notarget, "vk_setpos ...; save
# hl68v_<view>", which keeps the pitch) in the portals folder; a run then loads each view's save,
# pauses and shoots it: Hexenlicht's vk_screenshot <view> of 8 frames averaged, glh2's
# screenshot (renamed to <view>.tga). +lookup is held at cl_pitchspeed 0 throughout: it stops
# view.c's pitch drift (V_DriftPitch), which turns a loaded view level within a second on the
# ground, without turning the view. -Exe hexenlicht|glh2; -Bin another build (e.g. main's);
# -HlCvars Hexenlicht's cvars after each load (e.g. "r_emissive_models 0"); -Only a pattern of
# view names; -DeleteSaves removes the saves. config.cfg and hexenlicht.cfg of data1 and portals
# are backed up and restored, the folder's own shots moved aside and back, the scripts deleted.
# The shots, the log and the validation line go to -Out\<Tag>.
param([Parameter(Mandatory)][string]$Out, [string]$Tag = 'run', [ValidateSet('hexenlicht', 'glh2')][string]$Exe = 'hexenlicht',
      [string]$Bin = '', [switch]$Release, [switch]$Saves, [switch]$DeleteSaves, [string]$HlCvars = '', [string]$Only = '',
      [int]$Width = 960, [int]$Height = 540, [string]$Data = '')
$ErrorActionPreference = 'Stop'
if ($Saves -and $Exe -ne 'hexenlicht') { throw "-Saves needs Hexenlicht (vk_setpos)" }
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'portals'
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, the camera (x y z pitch yaw: the player's origin; pitch below 0 looks up)
$views = @(
	@('t1_burner', 'tibet1', '1488 300 -47 -12 90'),	# a light_burner: flame2.mdl 6 units over the light
	@('t1_row', 'tibet1', '2600 990 -547 0 180'),		# burners in a row, one close
	@('t1_hall', 'tibet1', '3500 66 -640 -8 0'),		# the hall of twelve burners
	@('t6_candles', 'tibet6', '-1500 927 128 -5 0'),	# light_candle: the model at the light
	@('t7_newfire', 'tibet7', '290 -1650 0 10 90'),		# light_newfire: translucent, fire flicker
	@('t8_torch_n', 'tibet8', '-1025 -1575 -8 -35 90'),	# light_palace_torch: flame2.mdl 32 units over the light
	@('t8_torch_s', 'tibet8', '-1024 -2334 -27 -35 270'),
	@('k2_lantern', 'keep2', '-559 1421 136 -40 0')		# light_lantern
)
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

if ($DeleteSaves) {
	foreach ($v in $views) { $s = Join-Path $game "hl68v_$($v[0])"; if (Test-Path $s) { Remove-Item -LiteralPath $s -Recurse } }
	return
}

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'playerclass 5', 'skill 1', 'lookspring 0', 'cl_pitchspeed 0', '+lookup')
$end = @('-lookup', 'toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')

# a script per view (the command buffer holds 8 KB)
$blocks = @()
if ($Saves) {
	$map = ''
	foreach ($v in $views) {
		$b = @()
		if ($v[1] -ne $map) { $b += @("map $($v[1])", (Waits 150), 'god', 'notarget', (Waits 30)); $map = $v[1] }
		$b += @("vk_setpos $($v[2]); save hl68v_$($v[0])", (Waits 10))
		$blocks += , $b
	}
} else {
	foreach ($v in $views) {
		$b = @("load hl68v_$($v[0])", (Waits 150))
		if ($Exe -eq 'hexenlicht' -and $HlCvars) { $b += $HlCvars, (Waits 10) }
		$b += @('pause', (Waits 30))
		$b += $(if ($Exe -eq 'glh2') { @('screenshot', (Waits 10)) } else { @("vk_screenshot $($v[0]) 8", (Waits 30)) })
		$blocks += , ($b + @('pause', (Waits 5)))
	}
}

$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl68_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$dst = Join-Path $Out $Tag
if ((Test-Path $dst) -and (Get-ChildItem $dst -File -Filter *.tga)) { throw "$dst has shots already: use a new -Tag" }
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
$cfgs = foreach ($d in 'data1', 'portals') { foreach ($c in 'config.cfg', 'hexenlicht.cfg') { Join-Path (Join-Path $Data $d) $c } }
function Backup([string]$c) { Join-Path $bk ((Split-Path (Split-Path $c) -Leaf) + '_' + (Split-Path $c -Leaf)) }
$existed = @{}
foreach ($c in $cfgs) { $existed[$c] = Test-Path $c; if ($existed[$c]) { Copy-Item $c (Backup $c) -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
Get-ChildItem $shots -File | Move-Item -Destination $aside
$files = @()
try {
	for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl68_$i.cfg"; [IO.File]::WriteAllText($f, $texts[$i]); $files += $f }
	$a = @{ Exe = $Exe; Cfg = 'hl68_0.cfg'; Portals = $true; Timeout = 900; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	if ($Exe -eq 'glh2') {	# hexen00.tga ... in the views' order
		$n = 0
		foreach ($v in $views) {
			$f = Join-Path $shots ('hexen{0:d2}.tga' -f $n++)
			if (Test-Path $f) { Move-Item $f (Join-Path $shots "$($v[0]).tga") }
		}
	}
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	# the configs and scripts first: a failed move of the shots mustn't keep them; a config
	# the game wrote where none was goes (the run's sensitivity 0, viewsize 130, ...)
	foreach ($c in $cfgs) {
		if ($existed[$c]) { Copy-Item (Backup $c) $c -Force }
		elseif (Test-Path $c) { [IO.File]::Delete($c) }
	}
	foreach ($f in $files) { Remove-Item -LiteralPath $f -ErrorAction Continue }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
Get-Content (Join-Path $dst 'debug_h2.log') | Select-String '^Vulkan validation|^Saving game|^Couldn|^ERROR'
