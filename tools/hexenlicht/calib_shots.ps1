# Matched screenshots of glh2 and Hexenlicht at the calibration bookmarks
# (story 4.9; docs/hexenlicht/TESTING.md, "Calibration against GL").
#  1. Saves: a Hexenlicht run goes to each bookmark (map, god, notarget,
#     noclip, vk_setpos) and saves there (hlcal_<name> in the game folder);
#     both engines then load the same save, so they show the same camera
#     and scene.
#  2. glh2: <name>_gl.tga (the image) and <name>_gllm.tga (r_lightmap 1: the
#     lightmaps alone, entities hidden) into <Out>\gl; with -GlLit <folder>
#     (Hammer of Thyrion's colored light: <folder>\maps\<map>.lit, copied
#     into the game folder's maps\ for the run and removed after) also
#     <name>_glc.tga, the image with gl_coloredlight 1 (Hexenlicht's colors,
#     4.3), for the maps that have one.
#  3. Hexenlicht: <name>_lit.tga (the lit image) and <name>_direct.tga (the
#     direct diffuse light, r_debugview 15, white lights, no denoiser,
#     entities hidden, lava and sky light off, times -Scale, averaged over
#     -Frames frames) into <Out>\<Label>, with -HlCvars set after each load
#     (e.g. "r_maplight_power 4; r_maplight_range 1.25"); -LitFrames averages
#     the lit image too (1: one frame, as seen).
# -PreSave runs console commands before each save (80 frames before it, 4.10: a
# lit torch, "impulse 43; <20 waits>; invuse", or an artifact, which the save
# keeps for both engines); -PlayerClass is the class the saves are made with (2,
# the Crusader; 3 the Necromancer); -Bin another build's folder for the
# Hexenlicht runs (hl_run.ps1's, e.g. a copy of main's for a comparison).
# All shots are paused, GL's without its view blends (gl_polyblend 0: the
# power-up tints and damage flashes, which Hexenlicht doesn't draw until 6.6),
# without the HUD, the weapon, the crosshair or the
# notify lines, at -Width x -Height. -Skip... leaves out a step (the saves
# are kept for later runs with -KeepSaves). config.cfg and hexenlicht.cfg
# of the game folder and data1 are backed up and restored around the runs
# (hexenlicht.cfg kept out of them), the folder's own numbered shots moved
# aside for glh2's and back; the
# scripts and (unless -KeepSaves) the saves are deleted afterwards.
param([string]$Bookmarks = '', [string[]]$Names = @(), [Parameter(Mandatory)][string]$Out, [string]$Label = 'hl',
      [string]$HlCvars = '', [int]$Frames = 16, [double]$Scale = 0.25, [switch]$SkipSaves, [switch]$SkipGl,
      [switch]$SkipHl, [switch]$KeepSaves, [int]$Width = 960, [int]$Height = 540, [string]$Data = '',
      [string]$GlLit = '', [int]$LitFrames = 1, [switch]$DebugBuild, [string]$PreSave = '', [int]$PlayerClass = 2,
      [string]$Bin = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Bookmarks) { $Bookmarks = Join-Path $PSScriptRoot 'bookmarks.txt' }
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$run = Join-Path $PSScriptRoot 'hl_run.ps1'
$inv = [Globalization.CultureInfo]::InvariantCulture
# a bookmark's block must fit into the engine's 8 KB command buffer
if ($Frames -lt 1 -or $Frames -gt 256 -or $LitFrames -lt 1 -or $LitFrames -gt 256) { throw "-Frames and -LitFrames: 1-256" }

# name map x y z pitch yaw [portals]
$marks = foreach ($line in Get-Content $Bookmarks) {
	$t = $line.Trim()
	if (-not $t -or $t.StartsWith('#') -or $t.StartsWith('//')) { continue }
	$w = $t -split '\s+'
	if ($w.Count -lt 7) { throw "bad bookmark line: $t" }
	[pscustomobject]@{ Name = $w[0]; Map = $w[1]; Pos = ($w[2..4] -join ' '); Pitch = $w[5]; Yaw = $w[6];
			   Game = if ($w.Count -gt 7 -and $w[7] -eq 'portals') { 'portals' } else { 'data1' } }
}
if ($Names.Count) { $marks = $marks | Where-Object { $n = $_.Name; $Names | Where-Object { $n -like $_ } } }
if (-not $marks) { throw "no bookmarks match" }

function Waits([int]$n) { (@('wait') * $n) -join ';' }

# the blocks into test scripts of at most ~6 KB (the command buffer holds 8),
# each exec'ing the next; returns the first script's name
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

# runs an engine with a script; config.cfg and hexenlicht.cfg of the game
# folder and data1 are restored afterwards, and no hexenlicht.cfg is there
# for the run (Hexenlicht's "exec config.cfg" finds it in data1 too, and
# archived cvars of earlier tests would come in)
function Invoke-Engine([string]$exe, [string]$game, [string]$cfg) {
	$saved = @{}
	foreach ($d in @($game, 'data1') | Select-Object -Unique) {
		foreach ($f in 'config.cfg', 'hexenlicht.cfg') {
			$p = Join-Path (Join-Path $Data $d) $f
			if (Test-Path $p) { $saved[$p] = [IO.File]::ReadAllBytes($p) } else { $saved[$p] = $null }	# an empty file stays byte[0]
			if ($f -eq 'hexenlicht.cfg' -and (Test-Path $p)) { [IO.File]::Delete($p) }
		}
	}
	try {
		$p = @{ Exe = $exe; Cfg = $cfg; Width = $Width; Height = $Height; Timeout = 1800; Data = $Data }
		if (-not $DebugBuild) { $p.Release = $true }
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
}

# a glh2 run whose numbered shots (shots\hexenNN.tga, in order) become
# $dests; the game folder's own numbered shots are moved aside for it and
# back after
function Invoke-GlShots([string]$game, [string]$cfg, [string[]]$dests) {
	$shots = Join-Path (Join-Path $Data $game) 'shots'; $aside = Join-Path $shots 'hlcal_kept'
	if ((Test-Path $aside) -and (Get-ChildItem $aside)) { throw "$aside holds shots an earlier run moved aside: move them back first" }
	$kept = @(Get-ChildItem $shots -Filter 'hexen*.tga'); $moved = @()
	try {
		if ($kept.Count) { New-Item -ItemType Directory -Force $aside | Out-Null }
		foreach ($k in $kept) { Move-Item $k.FullName (Join-Path $aside $k.Name); $moved += $k }
		Invoke-Engine 'glh2' $game $cfg
		$files = @(Get-ChildItem $shots -Filter 'hexen*.tga' | Sort-Object Name)
		if ($files.Count -ne $dests.Count) { throw "glh2 wrote $($files.Count) shots, expected $($dests.Count)" }
		for ($i = 0; $i -lt $dests.Count; $i++) { Move-Item $files[$i].FullName $dests[$i] -Force }
	} finally {
		# a failed run's shots, only once all of the folder's own are aside
		if ($moved.Count -eq $kept.Count) {
			Get-ChildItem $shots -Filter 'hexen*.tga' | ForEach-Object { [IO.File]::Delete($_.FullName) }
		}
		foreach ($k in $moved) { Move-Item (Join-Path $aside $k.Name) $k.FullName }
		if ((Test-Path $aside) -and -not (Get-ChildItem $aside)) { [IO.Directory]::Delete($aside) }
	}
}

$common = @('wait;wait;wait', 'host_framerate 0.02', 'viewsize 130', 'showpause 0', 'r_drawviewmodel 0', 'crosshair 0',
	    'con_notifytime 0', 'gamma 1')
$quit = @('toggleconsole', (Waits 5), 'quit')
$gldir = Join-Path $Out 'gl'; $hldir = Join-Path $Out $Label
New-Item -ItemType Directory -Force $gldir, $hldir | Out-Null

foreach ($group in ($marks | Group-Object Game)) {
	$game = $group.Name; $dir = Join-Path $Data $game; $shots = Join-Path $dir 'shots'
	$list = @($group.Group)
	New-Item -ItemType Directory -Force $shots | Out-Null

	if (-not $SkipSaves) {
		$blocks = foreach ($m in $list) {
			@("map $($m.Map)", (Waits 150), 'god', 'notarget', 'noclip', (Waits 5), $PreSave, (Waits $(if ($PreSave) { 80 } else { 0 })),
			  "vk_setpos $($m.Pos) $($m.Pitch) $($m.Yaw); save hlcal_$($m.Name)", (Waits 5)) -join "`n"
		}
		$first = Write-Scripts $dir 'hlcal_s' (@('wait;wait;wait', 'vid_vsync 0', 'host_framerate 0.02', "playerclass $PlayerClass")) $blocks $quit
		try { Invoke-Engine 'hexenlicht' $game $first } finally { Remove-Scripts $dir 'hlcal_s' }
		foreach ($m in $list) {
			$info = Join-Path $dir "hlcal_$($m.Name)\info.dat"
			if (-not (Test-Path $info)) { throw "no save for $($m.Name)" }
			# its map name (a map that didn't load leaves the last one's)
			if (-not (Get-Content $info | Where-Object { $_.Trim() -eq $m.Map })) { throw "the save for $($m.Name) isn't of $($m.Map)" }
		}
	}

	if (-not $SkipGl) {
		if ($list.Count -gt 50) { throw "glh2 numbers its shots 00-99: at most 50 bookmarks per game" }
		$blocks = foreach ($m in $list) {
			@("load hlcal_$($m.Name)", (Waits 100), 'pause', (Waits 10), 'screenshot', (Waits 5),
			  'r_lightmap 1', 'r_drawentities 0', (Waits 5), 'screenshot', (Waits 5),
			  'r_lightmap 0', 'r_drawentities 1', 'pause', (Waits 5)) -join "`n"
		}
		$dests = foreach ($m in $list) { (Join-Path $gldir "$($m.Name)_gl.tga"), (Join-Path $gldir "$($m.Name)_gllm.tga") }
		$first = Write-Scripts $dir 'hlcal_g' ($common + @('gl_coloredlight 0', 'gl_polyblend 0')) $blocks $quit
		try { Invoke-GlShots $game $first @($dests) } finally { Remove-Scripts $dir 'hlcal_g' }
	}

	if ($GlLit) {
		$maps = Join-Path $dir 'maps'; $made = -not (Test-Path $maps)
		$copied = @(); $lit = @()
		try {
			New-Item -ItemType Directory -Force $maps | Out-Null
			foreach ($m in $list) {
				$src = Join-Path $GlLit "maps\$($m.Map).lit"; $dst = Join-Path $maps "$($m.Map).lit"
				if (-not (Test-Path $src)) { continue }
				$lit += $m
				if (-not (Test-Path $dst)) { Copy-Item $src $dst; $copied += $dst }
			}
			if ($lit.Count) {
				$blocks = foreach ($m in $lit) {
					@("load hlcal_$($m.Name)", (Waits 100), 'pause', (Waits 10), 'screenshot', (Waits 5), 'pause', (Waits 5)) -join "`n"
				}
				$dests = foreach ($m in $lit) { Join-Path $gldir "$($m.Name)_glc.tga" }
				$first = Write-Scripts $dir 'hlcal_c' ($common + @('gl_coloredlight 1', 'gl_polyblend 0')) $blocks $quit
				try { Invoke-GlShots $game $first @($dests) } finally { Remove-Scripts $dir 'hlcal_c' }
			}
		} finally {
			foreach ($f in $copied) { [IO.File]::Delete($f) }
			if ($made -and (Test-Path $maps) -and -not (Get-ChildItem $maps)) { [IO.Directory]::Delete($maps) }
		}
	}

	if (-not $SkipHl) {
		$scale = $Scale.ToString($inv)
		$blocks = foreach ($m in $list) {
			@("load hlcal_$($m.Name)", (Waits 100), $HlCvars, 'pause',
			  'tm_exposure_speed_up 100; tm_exposure_speed_down 100', (Waits 60),
			  "vk_screenshot $($m.Name)_lit $LitFrames", (Waits ($LitFrames + 5)),
			  'r_drawentities 0; r_maplight_colors 0; flt_enable 0; r_lava_light 0; r_sky_light 0; r_sun 0',
			  "r_debugview 15; r_debugview_scale $scale", (Waits 10),
			  "vk_screenshot $($m.Name)_direct $Frames", (Waits ($Frames + 10)),
			  'r_debugview 0; r_debugview_scale 1; r_drawentities 1; r_maplight_colors 0; flt_enable 1; r_lava_light 1',
			  'tm_exposure_speed_up 2; tm_exposure_speed_down 1', 'pause', (Waits 5)) -join "`n"
		}
		$first = Write-Scripts $dir 'hlcal_h' ($common + 'vid_vsync 0') $blocks $quit
		try { Invoke-Engine 'hexenlicht' $game $first } finally { Remove-Scripts $dir 'hlcal_h' }
		foreach ($m in $list) {
			foreach ($kind in 'lit', 'direct') {
				$f = Join-Path $shots "$($m.Name)_$kind.tga"
				if (-not (Test-Path $f)) { throw "Hexenlicht wrote no $($m.Name)_$kind.tga" }
				Move-Item $f (Join-Path $hldir "$($m.Name)_$kind.tga") -Force
			}
		}
	}

	if (-not $KeepSaves) {
		foreach ($m in $list) {
			$s = Join-Path $dir "hlcal_$($m.Name)"
			if (Test-Path $s) { [IO.Directory]::Delete($s, $true) }
		}
	}
}
"shots in $Out (gl, $Label): $($marks.Count) bookmarks"
