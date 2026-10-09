# The in-game shots of the census's views (story 9.2; docs/hexenlicht/TESTING.md,
# "Texture census and views (9.2)"): per line of -Views (`texpack.py views`:
# `<texture> <map> <close: x y z pitch yaw> <wide: x y z pitch yaw>`) the original
# textures (r_materials 0) at the close and the wide view, each paused and 8 frames
# averaged with `vk_materials here` after it, and the wide view's albedo (r_debugview 1,
# unlit: what is there where the light is dark). Each map loads once (the lines come by
# map); god, notarget and noclip, the view weapon hidden, `vk_benchmark 1` (no 72 fps cap).
# -Data is a game data folder of its own, never the one with your textures: a copy of the
# original's data1 (pak0.pak, pak1.pak, PROGS.DAT, PROGS2.DAT, Strings.txt, Hexen.rc) and,
# for the mission pack's maps, portals (pak3.pak, progs.dat, hexen.rc, default.cfg, strings.txt,
# puzzles.txt, infolist.txt), without textures\ and config files; -Portals runs those maps
# (`texpack.py views --game portals`: its first line names the game, and a file of the other
# game's maps is refused). config.cfg and hexenlicht.cfg are backed up and
# restored, the folder's shots moved aside and back, the scripts deleted (only the files
# this script wrote). The shots (TGA), views.csv (each shot's name and its texture) and the
# log go to -Out\<Tag>; `texpack.py checkviews` reads them. Run it with PowerShell 7 (the
# pwsh tool), Debug unless -Release; -Bin another build; -Only a regex on the texture names;
# -Settle the frames waited before and after the pause at each view (40; rounded to 20s).
param([Parameter(Mandatory)][string]$Views, [Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$Data,
      [string]$Tag = 'views', [switch]$Portals, [switch]$Release, [string]$Only = '', [int]$Settle = 40,
      [int]$Width = 1280, [int]$Height = 720, [string]$Bin = '', [int]$Timeout = 14400)
$ErrorActionPreference = 'Stop'
$Data = (Resolve-Path -LiteralPath $Data).ProviderPath
$game = Join-Path $Data 'data1'
$gdir = if ($Portals) { Join-Path $Data 'portals' } else { $game }	# where the game writes its configs and shots
$run = Join-Path (Split-Path $PSScriptRoot) 'hl_run.ps1'

# the views; a shot's name keeps a texture name's letters, digits, _ and - (vk_screenshot's rule)
$list = @()
$names = @{}
$viewsFor = ''		# `texpack.py views` names the game its maps are in: a run of each, portals with -Portals
foreach ($ln in Get-Content -LiteralPath $Views) {
	if ($ln -match '^# game (\S+?):') { $viewsFor = $Matches[1]; continue }
	$t = ($ln -split '\s+#\s')[0].Trim() -split '\s+'	# a comment is '# ' (#lava000 is a name)
	if ($ln -match '^\s*#\s' -or $t.Count -lt 12) { continue }
	if ($Only -and $t[0] -notmatch $Only) { continue }
	$name = $t[0] -replace '#', '_' -replace '\+', 'p' -replace '~', '-' -replace '[^A-Za-z0-9_-]', '_'
	if ($names.ContainsKey($name)) { throw "$($t[0]) and $($names[$name]) both make the shot name $name" }
	$names[$name] = $t[0]
	$list += , [pscustomobject]@{ name = $name; stem = $t[0]; map = $t[1]; close = ($t[2..6] -join ' '); wide = ($t[7..11] -join ' ') }
}
if (-not $list.Count) { throw "no views in $Views" + $(if ($Only) { " matching -Only '$Only'" } else { '' }) }
if ($Portals -and -not (Test-Path (Join-Path $Data 'portals\pak3.pak'))) { throw "-Portals: $Data has no portals\pak3.pak" }
if ($viewsFor -and (($viewsFor -eq 'portals') -ne [bool]$Portals)) { throw "$Views has the views of $viewsFor's maps: $(if ($Portals) { 'without' } else { 'with' }) -Portals" }

# the scripts: aliases for the waits keep each under the command buffer (TESTING.md), a new
# file when one would grow past 7000 bytes
$start = @('alias w5 "wait;wait;wait;wait;wait"', 'alias w20 "w5;w5;w5;w5"', 'w5', 'vid_vsync 0', 'vk_benchmark 1',
	   'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0', 'con_notifytime 0',
	   'r_drawviewmodel 0', 'r_materials 0', 'color 0 0', 'skill 1')
$end = @('echo T92_END', 'vk_benchmark 0', 'toggleconsole', 'w5', 'vid_vsync 1', 'quit')
$settleWaits = (@('w20') * [Math]::Max(1, [int][Math]::Round($Settle / 20))) -join ';'
$blocks = @(); $lastMap = ''
foreach ($v in $list) {
	$b = @()
	if ($v.map -ne $lastMap) {
		$b += @('playerclass 1', "map $($v.map)", 'w20;w20;w20;w20;w20;w20;w20', 'god', 'notarget', 'noclip'); $lastMap = $v.map
	}
	# the lit shots: moved, -Settle frames for the client and the denoiser, paused, -Settle more, 8 averaged
	foreach ($w in @(@('close', $v.close), @('wide', $v.wide))) {
		$b += @("vk_setpos $($w[1])", $settleWaits, 'pause', $settleWaits, "vk_screenshot $($v.name)_$($w[0]) 8", 'w5;w5;wait;wait',
			"echo T92_VIEW $($v.name) $($w[0])", 'vk_materials here')
		if ($w[0] -eq 'wide') { $b += @('r_debugview 1', 'w5;w5', "vk_screenshot $($v.name)_albedo 1", 'w5', 'r_debugview 0') }
		$b += @('pause', 'wait')
	}
	$blocks += , $b
}
$texts = @(); $cur = @()
foreach ($b in $blocks) {
	if ($cur.Count -and ((($cur + $b) -join "`r`n").Length -gt 7000)) { $texts += , $cur; $cur = @() }
	$cur += $b
}
$texts += , $cur
$files = @()
for ($i = 0; $i -lt $texts.Count; $i++) {
	$lines = $texts[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $texts.Count - 1) { "exec hl92_$($i + 1).cfg" } else { $end })
	$text = ($lines -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$files += $text
}

$dst = Join-Path $Out $Tag
if (Test-Path $dst) { throw "$dst exists: a fresh -Tag keeps an old run's config backups from being restored" }
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
$list | Select-Object name, stem, map, close, wide | Export-Csv -LiteralPath (Join-Path $dst 'views.csv') -NoTypeInformation -Encoding utf8NoBOM
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $gdir $c)) { Copy-Item (Join-Path $gdir $c) $bk -Force } }
$made = @('config.cfg', 'hexenlicht.cfg' | Where-Object { -not (Test-Path (Join-Path $gdir $_)) })	# the game writes them at quit
$shots = Join-Path $gdir 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
$written = New-Object System.Collections.Generic.List[string]
$t0 = Get-Date
try {
	Get-ChildItem $shots -File | Move-Item -Destination $aside
	for ($i = 0; $i -lt $files.Count; $i++) { $f = Join-Path $game "hl92_$i.cfg"; $written.Add($f); [IO.File]::WriteAllText($f, $files[$i]) }
	"$($list.Count) views in $(@($list | ForEach-Object map | Select-Object -Unique).Count) maps, $($files.Count) scripts"
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl92_0.cfg'; Timeout = $Timeout; Width = $Width; Height = $Height; Data = $Data }
	if ($Portals) { $a.Portals = $true }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
} finally {
	# each step on its own, so one that fails (a file still held) doesn't keep the others from
	# running: the configs and scripts first, then this run's shots and log (a run that stopped
	# keeps what it shot; a log older than the run is the last run's), the folder's own shots back
	$ErrorActionPreference = 'Continue'
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $gdir -Force } }
	foreach ($c in $made) { $p = Join-Path $gdir $c; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p } }
	foreach ($f in $written) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f } }
	Get-ChildItem $shots -File -Filter '*.tga' | Where-Object { $_.LastWriteTime -ge $t0 } | Move-Item -Destination $dst -Force
	$lg = Get-Item -LiteralPath (Join-Path $Data 'debug_h2.log') -ErrorAction SilentlyContinue
	if ($lg -and $lg.LastWriteTime -ge $t0) { Copy-Item -LiteralPath $lg.FullName (Join-Path $dst 'debug_h2.log') -Force }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
	$ErrorActionPreference = 'Stop'
}
"$([int]((Get-Date) - $t0).TotalSeconds) s"
if (-not (Test-Path (Join-Path $dst 'debug_h2.log'))) { throw "no log of this run: did the game start?" }
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation'
"$(@($log | Select-String '^T92_VIEW \S+ close\s*$').Count) of $($list.Count) views shot; $(@(Get-ChildItem $dst -Filter '*.tga').Count) shots"
