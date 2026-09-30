# Scripted effects run (story 6.1; docs/hexenlicht/TESTING.md, "Effects (6.1)"):
# every class weapon, normal and with the tome of power, and the artifacts, in
# glh2 and Hexenlicht from the same saves, with paused shots named by effect.
#  1. Saves: a Hexenlicht run per class (-Class; 5, the Demoness, runs with
#     -portals) starts -Map, gives god, notarget, every weapon, mana and item
#     (impulse 43) and saves at -Near and -Far (x y z pitch yaw; hlfx_c<n>n,
#     hlfx_c<n>f in the game folder). A load levels the view: the near
#     steps then look down -NearPitch degrees (+lookdown at cl_pitchspeed
#     100, 2 degrees a frame), so the melee weapons (64 units) reach the
#     pedestal.
#  2. Each engine (-Exe hexenlicht, glh2 or both) runs the steps, each from
#     a fresh load of its save: near w1-w4 and, with the tome (impulse 25),
#     w1t-w4t; the same from far; then from far each of -Items (impulse 99 +
#     its inventory number). A weapon step selects the weapon, holds +attack
#     for -Burst frames and shoots paused at each of -Delays frames after it
#     started (game time: host_framerate 0.02; the sunstaff fires ~10 frames
#     after the button); an item step shoots at -ItemDelays. Shots:
#     <Out>\<exe>\c<n>_<near|far|item>_<step>_<delay>.tga.
#     Each run's log is <Out>\<exe>\c<n>.log; Hexenlicht's has vk_effects and
#     vk_models after each step (what was left out for lack of room since the
#     step's load): the summary prints the frames with effects left out, the
#     instances left out and the validation lines (the saves' run's too).
# -HlCvars (6.2) sets Hexenlicht's cvars after each load (e.g. "r_effect_lights
# 0" for a run to compare with; glh2 runs without them).
# -NoPause (6.2) takes the shots with the game running: a paused game draws
# no client effects (the CE_* sprites and models: host.c runs CL_UpdateEffects
# only while the server runs), so paused shots show only the server's
# entities. Game time is fixed (host_framerate), so two runs from the same
# saves show the same moments (particles and chunks still fly differently);
# the denoiser has less history than in a paused shot.
# -SkipSaves uses saves an earlier run kept (-KeepSaves).
# The Debug build by default (validation), -Release for speed; -Bin another
# build's folder for the Hexenlicht runs. config.cfg and hexenlicht.cfg of the
# game folder and data1 are backed up and restored around each run
# (hexenlicht.cfg kept out of it), the folder's own numbered shots moved aside
# and back; the scripts and (unless -KeepSaves) the saves are deleted after.
param([int[]]$Class = @(1, 2, 3, 4), [Parameter(Mandatory)][string]$Out,
      [ValidateSet('hexenlicht', 'glh2', 'both')][string]$Exe = 'both',
      [string]$Map = 'demo1', [string]$Near = '-780 -1896 0 0 45', [string]$Far = '-918 -2034 0 0 45', [int]$NearPitch = 20,
      [int[]]$Delays = @(12, 30, 50), [int[]]$ItemDelays = @(10, 60, 200), [int]$Burst = 30,
      [string[]]$Items = @('torch=1', 'summon=7', 'invisibility=8', 'glyph=9', 'haste=10', 'blast=11',
			   'polymorph=12', 'cube=14', 'invincibility=15', 'teleport=5'),
      [switch]$SkipSaves, [switch]$KeepSaves, [switch]$Release, [int]$Width = 960, [int]$Height = 540,
      [string]$Data = '', [string]$Bin = '', [string]$HlCvars = '', [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
foreach ($c in $Class) { if ($c -lt 1 -or $c -gt 5) { throw "-Class: 1-5" } }
foreach ($d in $Delays + $ItemDelays) { if ($d -lt 1) { throw "-Delays and -ItemDelays: at least 1 frame" } }
if ($NearPitch -lt 0 -or $NearPitch -gt 80 -or $NearPitch % 2) { throw "-NearPitch: an even number of degrees, 0-80" }
$itemList = foreach ($i in $Items) {	# not $items: $Items is typed [string[]]
	$n, $v = $i -split '='
	if (-not $v -or [int]$v -lt 1 -or [int]$v -gt 15) { throw "bad item '$i': name=inventory number (1-15)" }
	[pscustomobject]@{ Name = $n; Impulse = 99 + [int]$v }
}
$exes = if ($Exe -eq 'both') { 'hexenlicht', 'glh2' } else { , $Exe }

function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# the blocks into test scripts of at most ~6 KB (the command buffer holds 8),
# each exec'ing the next; returns the first script's name (as calib_shots.ps1)
function Write-Scripts([string]$dir, [string]$prefix, [string[]]$header, [string[]]$blocks, [string[]]$footer) {
	$files = @(); $cur = [Collections.Generic.List[string]]::new(); $size = 0
	foreach ($b in (@($header -join "`n") + $blocks)) {
		if ($size + $b.Length -gt 6000 -and $cur.Count) { $files += , $cur.ToArray(); $cur.Clear(); $size = 0 }
		$cur.Add($b); $size += $b.Length + 1
	}
	$cur.Add($footer -join "`n"); $files += , $cur.ToArray()
	for ($i = 0; $i -lt $files.Count; $i++) {
		$text = $files[$i] -join "`n"
		if ($i + 1 -lt $files.Count) { $text += "`nexec $prefix$($i + 1).cfg" }
		Set-Content -Path (Join-Path $dir "$prefix$i.cfg") -Value $text -NoNewline:$false
	}
	"${prefix}0.cfg"
}

function Remove-Scripts([string]$dir, [string]$prefix) {
	Get-ChildItem $dir -Filter "$prefix*.cfg" | ForEach-Object { [IO.File]::Delete($_.FullName) }
}

# runs an engine with a script, config.cfg and hexenlicht.cfg of the game
# folder and data1 restored afterwards and no hexenlicht.cfg there for the run
# (as calib_shots.ps1); returns the run's log
function Invoke-Engine([string]$exe, [string]$game, [string]$cfg) {
	$saved = @{}
	foreach ($d in @($game, 'data1') | Select-Object -Unique) {
		foreach ($f in 'config.cfg', 'hexenlicht.cfg') {
			$p = Join-Path (Join-Path $Data $d) $f
			if (Test-Path $p) { $saved[$p] = [IO.File]::ReadAllBytes($p) } else { $saved[$p] = $null }
			if ($f -eq 'hexenlicht.cfg' -and (Test-Path $p)) { [IO.File]::Delete($p) }
		}
	}
	try {
		$p = @{ Exe = $exe; Cfg = $cfg; Width = $Width; Height = $Height; Timeout = 1800; Data = $Data }
		if ($Release) { $p.Release = $true }
		if ($game -eq 'portals') { $p.Portals = $true }
		if ($Bin -and $exe -eq 'hexenlicht') { $p.Bin = $Bin }
		$r = & $run @p
		if ($r -ne 'exit 0') { throw "$exe $cfg ended with: $r" }
	} finally {
		foreach ($p in $saved.Keys) {
			if ($null -ne $saved[$p]) { [IO.File]::WriteAllBytes($p, $saved[$p]) }
			elseif (Test-Path $p) { [IO.File]::Delete($p) }
		}
	}
	Get-Content (Join-Path $Data 'debug_h2.log')
}

# a run whose numbered shots (shots\hexenNN.tga, in order; both engines name
# them so) become $dests; the game folder's own numbered shots are moved aside
# for it and back after (as calib_shots.ps1's glh2 runs)
function Invoke-Shots([string]$exe, [string]$game, [string]$cfg, [string[]]$dests) {
	$shots = Join-Path (Join-Path $Data $game) 'shots'; $aside = Join-Path $shots 'hlfx_kept'
	New-Item -ItemType Directory -Force $shots | Out-Null
	if ((Test-Path $aside) -and (Get-ChildItem $aside)) { throw "$aside holds shots an earlier run moved aside: move them back first" }
	$kept = @(Get-ChildItem $shots -Filter 'hexen*.tga'); $moved = @()
	try {
		if ($kept.Count) { New-Item -ItemType Directory -Force $aside | Out-Null }
		foreach ($k in $kept) { Move-Item $k.FullName (Join-Path $aside $k.Name); $moved += $k }
		$log = Invoke-Engine $exe $game $cfg
		$files = @(Get-ChildItem $shots -Filter 'hexen*.tga' | Sort-Object Name)
		if ($files.Count -ne $dests.Count) { throw "$exe wrote $($files.Count) shots, expected $($dests.Count)" }
		for ($i = 0; $i -lt $dests.Count; $i++) { Move-Item $files[$i].FullName $dests[$i] -Force }
		$log
	} finally {
		if ($moved.Count -eq $kept.Count) {
			Get-ChildItem $shots -Filter 'hexen*.tga' | ForEach-Object { [IO.File]::Delete($_.FullName) }
		}
		foreach ($k in $moved) { Move-Item (Join-Path $aside $k.Name) $k.FullName }
		if ((Test-Path $aside) -and -not (Get-ChildItem $aside)) { [IO.Directory]::Delete($aside) }
	}
}

# one step: $start (e.g. +attack), $stop after $hold frames of game time, a
# shot at each delay, paused unless -NoPause (frames of game time since $start; shots don't
# advance it), then $settle frames; returns the block and the shots' names
function Step([string]$prelude, [string]$start, [string]$stop, [int]$hold, [int[]]$delays, [string]$name, [int]$settle) {
	$events = @()
	if ($stop) { $events += [pscustomobject]@{ At = $hold; Cmd = $stop; Shot = '' } }
	foreach ($d in $delays) { $events += [pscustomobject]@{ At = $d; Cmd = ''; Shot = '{0}_{1:d2}' -f $name, $d } }
	$lines = @($prelude, $start); $t = 0; $shots = @()
	foreach ($e in ($events | Sort-Object At, @{ Expression = { $_.Shot -ne '' } })) {
		$lines += Waits ($e.At - $t); $t = $e.At
		if ($e.Cmd) { $lines += $e.Cmd }
		elseif ($NoPause) { $lines += 'screenshot'; $shots += $e.Shot }	# the next frame, the game running
		else { $lines += 'pause', (Waits 8), 'screenshot', (Waits 5), 'pause'; $shots += $e.Shot }
	}
	$lines += Waits $settle
	[pscustomobject]@{ Block = (($lines | Where-Object { $_ }) -join "`n"); Shots = $shots }
}

# the view model, the look speed and HoT's glows at known values, whatever the configs say
$common = @('wait;wait;wait', 'vid_vsync 0', 'host_framerate 0.02', 'viewsize 130', 'showpause 0', 'crosshair 0',
	    'con_notifytime 0', 'gamma 1', 'scr_centertime 0', 'r_drawviewmodel 1', 'cl_pitchspeed 100; lookspring 0',
	    'gl_missile_glows 1; gl_glows 0; gl_other_glows 0')
$quit = @('toggleconsole', (Waits 5), 'quit')
$hlMark = '//hlcvars'	# replaced by -HlCvars in Hexenlicht's scripts, by nothing in glh2's
$summary = @()

foreach ($group in ($Class | Group-Object { if ($_ -eq 5) { 'portals' } else { 'data1' } })) {
	$game = $group.Name; $dir = Join-Path $Data $game

	if (-not $SkipSaves) {
		$blocks = foreach ($c in $group.Group) {
			@("playerclass $c", "map $Map", (Waits 150), 'god', 'notarget', 'impulse 43', (Waits 30),
			  "vk_setpos $Near; save hlfx_c${c}n", (Waits 5), "vk_setpos $Far; save hlfx_c${c}f", (Waits 5)) -join "`n"
		}
		$first = Write-Scripts $dir 'hlfx_s' $common $blocks $quit
		try { $log = Invoke-Engine 'hexenlicht' $game $first } finally { Remove-Scripts $dir 'hlfx_s' }
		$summary += "saves ($game): " + (($log | Where-Object { $_ -match '^Vulkan validation:' }) -join ' ')
	}
	# a missing save would leave the run waiting for its timeout
	foreach ($c in $group.Group) {
		foreach ($s in 'n', 'f') {
			$info = Join-Path $dir "hlfx_c$c$s\info.dat"
			if (-not (Test-Path $info)) { throw "no save hlfx_c$c$s" }
			if (-not (Get-Content $info | Where-Object { $_.Trim() -eq $Map })) { throw "the save hlfx_c$c$s isn't of $Map" }
		}
	}

	foreach ($c in $group.Group) {
		# every step loads its save: shots move the player and break things
		# (demo1's statue), and summoned creatures stay
		$steps = @()
		foreach ($spot in @(@('near', 'n'), @('far', 'f'))) {
			foreach ($tome in '', 't') {
				foreach ($w in 1..4) {
					$pre = @("load hlfx_c$c$($spot[1])", (Waits 100), $hlMark)
					if ($spot[0] -eq 'near' -and $NearPitch) { $pre += '+lookdown', (Waits ($NearPitch / 2)), '-lookdown' }
					if ($tome) { $pre += 'impulse 25', (Waits 40) }
					$pre += "impulse $w", (Waits 100)	# the last weapon's deselect and this one's select
					$steps += Step ($pre -join "`n") '+attack' '-attack' $Burst $Delays "c${c}_$($spot[0])_w$w$tome" 5
				}
			}
		}
		foreach ($i in $itemList) {
			$steps += Step "load hlfx_c${c}f`n$(Waits 100)`n$hlMark" "impulse $($i.Impulse)" '' 0 $ItemDelays "c${c}_item_$($i.Name)" 5
		}
		$names = @($steps | ForEach-Object { $_.Shots } | Where-Object { $_ })
		if ($names.Count -gt 100) { throw "class ${c}: $($names.Count) shots, the engines number at most 100" }
		foreach ($e in $exes) {
			$odir = Join-Path $Out $e
			New-Item -ItemType Directory -Force $odir | Out-Null
			# Hexenlicht counts what it left out since the last map load (a load
			# is one): after each step
			$tail = if ($e -eq 'hexenlicht') { "`nvk_effects`nvk_models" } else { '' }
			$cvars = if ($e -eq 'hexenlicht') { $HlCvars } else { '' }	# after each load (6.2)
			$first = Write-Scripts $dir 'hlfx_r' $common ($steps | ForEach-Object { $_.Block.Replace($hlMark, $cvars) + $tail }) $quit
			$dests = foreach ($n in $names) { Join-Path $odir "$n.tga" }
			try { $log = Invoke-Shots $e $game $first @($dests) } finally { Remove-Scripts $dir 'hlfx_r' }
			Set-Content (Join-Path $odir "c$c.log") $log
			$note = ''
			if ($e -eq 'hexenlicht') {
				# vk_effects: frames with effects left out; vk_models: instances left out
				$fx = 0; $inst = 0
				foreach ($l in $log) {
					if ($l -match '^left out: \d+ particles and \d+ sprites \(no room; in (\d+) frames since') { $fx += [int]$Matches[1] }
					if ($l -match '^left out: \d+ instances this frame, (\d+) since') { $inst += [int]$Matches[1] }
				}
				$note = "; frames with effects left out: $fx, instances left out: $inst; " +
					(($log | Where-Object { $_ -match '^Vulkan validation:' }) -join ' ')
			}
			$summary += "class $c $e`: $($names.Count) shots$note"
		}
	}

	if (-not $KeepSaves) {
		foreach ($c in $group.Group) {
			foreach ($s in 'n', 'f') {
				$p = Join-Path $dir "hlfx_c$c$s"
				if (Test-Path $p) { [IO.Directory]::Delete($p, $true) }
			}
		}
	}
}
$summary
"shots in $Out"
