# The in-game proof of a texpack pack (story 5.8; docs/hexenlicht/TESTING.md,
# "Texture pack tooling (5.8)"): copies -Pack's textures\ into data1\textures
# (refused if that has files), runs views of demo1 and meso9 (-Only to pick),
# and at each shoots the originals (r_materials 0), the pack (r_materials 1) and,
# at some, the shading normals (r_debugview 2) and the roughness (10) of the pack,
# 8 frames averaged, paused. `vk_materials here` names each view's texture;
# `vk_materials problems` and the validation line end the log. config.cfg and
# hexenlicht.cfg of data1 are backed up and restored, the folder's own shots moved
# aside and back, the pack's files and the scripts deleted (only the files this
# script wrote). The shots, the log and a list of the files go to -Out\<Tag>.
# -Data is a game data folder of its own, never the one with your textures: a
# copy of the original's data1 (pak0.pak, pak1.pak, PROGS.DAT, PROGS2.DAT,
# Strings.txt, Hexen.rc) with no textures\ and no config files. Run it with
# PowerShell 7 (the pwsh tool), Debug unless -Release; -Bin another build.
param([Parameter(Mandatory)][string]$Pack, [Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$Data,
      [string]$Tag = 'proof', [string]$Only = '', [switch]$Creatures, [switch]$Release, [int]$Width = 1280, [int]$Height = 720, [string]$Bin = '')
$ErrorActionPreference = 'Stop'
$Data = (Resolve-Path -LiteralPath $Data).ProviderPath
$game = Join-Path $Data 'data1'
$run = Join-Path (Split-Path $PSScriptRoot) 'hl_run.ps1'
$Pack = (Resolve-Path -LiteralPath $Pack).ProviderPath
function Waits([int]$n) { if ($n -le 0) { return '' }; (@('wait') * $n) -join ';' }

# name, map, class (1 Paladin), the place (x y z pitch yaw: test_pack.ps1's views), debug shots (2 normals, 10 roughness)
$views = @(
	@('start', 'demo1', 1, '-918 -2034 0 0 90', @()),
	@('wall', 'demo1', 1, '-1604 452 110 0 135', @(2, 10)),
	@('cobbles', 'demo1', 1, '-1781 -1321 -37 49 -163', @(2, 10)),
	@('metal', 'demo1', 1, '408 -1360 -214 0 90', @(10)),
	@('metal_side', 'demo1', 1, '300 -1400 -214 0 45', @()),
	@('pool', 'demo1', 1, '-1300 2416 -480 20 180', @()),
	@('water', 'demo1', 1, '-983 2300 -506 51 180', @()),
	@('runes', 'demo1', 1, '-608 -2120 374 0 0', @(2)),
	@('window_in', 'demo1', 1, '-280 -1972 234 0 0', @()),
	@('skylight', 'demo1', 1, '157 -1848 318 -51 180', @()),
	@('meso9', 'meso9', 1, '', @())
)
# -Creatures: demo1's courtyard (the start), a monster created 80 units ahead of the player at six
# headings (`create <classname>`: with god and notarget they stand still), each shot from 200 units
# behind the player's spot looking at it; the paladin's gauntlets are in every shot. Skins with a
# model name in the first column; the view weapon of the class, the monsters', or whatever -Pack has.
# A monster the map doesn't precache isn't created (demo1 has imps and archers): its view is an empty
# courtyard, so the log gets a `T58_CREATURE <view> <n> models` line from `vk_models` per view to read.
$cx, $cy, $cz = -918, -2034, 0
$creatureSet = @(
	@('imp', 'monster_imp_fire', 0, @(2)), @('golem', 'monster_golem_stone', 60, @(2, 10)), @('archer', 'monster_archer', 120, @()),
	@('mummy', 'monster_mummy', 180, @()), @('skullwiz', 'monster_skull_wizard', 240, @()), @('medusa', 'monster_medusa', 300, @()))
if ($Creatures) {
	$views = @()
	foreach ($c in $creatureSet) {
		$th = [double]$c[2] * [Math]::PI / 180
		$bx = [int]($cx - 120 * [Math]::Cos($th)); $by = [int]($cy - 120 * [Math]::Sin($th))
		$views += , @($c[0], 'demo1', 1, "$bx $by $cz 0 $($c[2])", $c[3], $c[1], $c[2])
	}
}
if ($Only) { $views = @($views | Where-Object { $_[0] -match $Only }) }
if (-not $views.Count) { throw "no view matches -Only '$Only'" }

$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'sensitivity 0', 'viewsize 130', 'showpause 0', 'crosshair 0',
	   'con_notifytime 0', 'r_maplight_shape 2', 'color 0 0', 'skill 1')
$end = @('echo T58_END', 'vk_materials problems', 'toggleconsole', (Waits 3), 'vid_vsync 1', 'quit')
$blocks = @(); $lastMap = ''
foreach ($v in $views) {
	$b = @()
	if ($v[1] -ne $lastMap) {
		$b += @("playerclass $($v[2])", "map $($v[1])", (Waits 150), 'god', 'notarget', 'noclip'); $lastMap = $v[1]
		if ($Creatures) {	# all the monsters first, one heading each; the views come after
			foreach ($c in $creatureSet) { if ($views | Where-Object { $_[0] -eq $c[0] }) { $b += @("vk_setpos $cx $cy $cz 0 $($c[2])", (Waits 5), "create $($c[1])", (Waits 15)) } }
			$b += (Waits 30)
		}
	}
	if ($v[3]) { $b += "vk_setpos $($v[3])" }
	if ($Creatures) { $b += @((Waits 60), "echo T58_MODELS $($v[0])", 'vk_models') }
	$b += @((Waits 60), 'pause', (Waits 10), 'r_debugview 0',
		'r_materials 0', (Waits 40), "vk_screenshot $($v[0])_orig 8", (Waits 12),
		'r_materials 1', (Waits 40), "vk_screenshot $($v[0])_pack 8", (Waits 12), "echo T58_VIEW $($v[0])", 'vk_materials here')
	foreach ($d in $v[4]) { $b += @("r_debugview $d", (Waits 40), "vk_screenshot $($v[0])_dbg$d 8", (Waits 12)) }
	$b += @('r_debugview 0', 'pause', (Waits 5))
	$blocks += , $b
}
$texts = @()
for ($i = 0; $i -lt $blocks.Count; $i++) {
	$lines = $blocks[$i]
	if ($i -eq 0) { $lines = $start + $lines }
	$lines += $(if ($i -lt $blocks.Count - 1) { "exec hl58_$($i + 1).cfg" } else { $end })
	$text = (@($lines | Where-Object { $_ -ne '' } | ForEach-Object { $_ }) -join "`r`n") + "`r`n"
	if ($text.Length -gt 7800) { throw "script $i is $($text.Length) bytes: over the command buffer (TESTING.md)" }
	$texts += $text
}
$texDir = Join-Path $game 'textures'
if ((Test-Path $texDir) -and (Get-ChildItem $texDir -Recurse -File)) { throw "data1\textures has files: remove them first (a pack, or a test set's -Remove)" }
$dst = Join-Path $Out $Tag
if (Test-Path $dst) { throw "$dst exists: a fresh -Tag keeps an old run's config backups from being restored" }
$bk = Join-Path $dst 'configs'; New-Item -ItemType Directory -Force $bk | Out-Null
foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $game $c)) { Copy-Item (Join-Path $game $c) $bk -Force } }
$shots = Join-Path $game 'shots'; New-Item -ItemType Directory -Force $shots | Out-Null
$aside = Join-Path $dst 'aside'; New-Item -ItemType Directory -Force $aside | Out-Null
$written = New-Object System.Collections.Generic.List[string]
try {
	Get-ChildItem $shots -File | Move-Item -Destination $aside
	for ($i = 0; $i -lt $texts.Count; $i++) { $f = Join-Path $game "hl58_$i.cfg"; $written.Add($f); [IO.File]::WriteAllText($f, $texts[$i]) }
	New-Item -ItemType Directory -Force $texDir | Out-Null
	foreach ($f in Get-ChildItem -LiteralPath (Join-Path $Pack 'textures') -Recurse -File) {
		$rel = $f.FullName.Substring((Join-Path $Pack 'textures').Length).TrimStart('\')
		$to = Join-Path $texDir $rel
		New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
		Copy-Item -LiteralPath $f.FullName -Destination $to
		$written.Add($to)
	}
	Set-Content -LiteralPath (Join-Path $dst 'files.txt') -Value $written
	"$($written.Count) files written (the pack's and the scripts')"
	$a = @{ Exe = 'hexenlicht'; Cfg = 'hl58_0.cfg'; Timeout = 2400; Width = $Width; Height = $Height; Data = $Data }
	if ($Bin) { $a.Bin = $Bin }
	if ($Release) { $a.Release = $true }
	& $run @a
	Get-ChildItem $shots -File | Move-Item -Destination $dst -Force
	Copy-Item (Join-Path $Data 'debug_h2.log') (Join-Path $dst 'debug_h2.log') -Force
} finally {
	# the configs and scripts first, then the pack's files; the folders only if empty
	foreach ($c in 'config.cfg', 'hexenlicht.cfg') { if (Test-Path (Join-Path $bk $c)) { Copy-Item (Join-Path $bk $c) $game -Force } }
	foreach ($f in $written) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -ErrorAction Continue } }
	foreach ($d in (Get-ChildItem -LiteralPath $texDir -Recurse -Directory -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending)) {
		if (-not (Get-ChildItem -LiteralPath $d.FullName)) { Remove-Item -LiteralPath $d.FullName }
	}
	if ((Test-Path $texDir) -and -not (Get-ChildItem -LiteralPath $texDir)) { Remove-Item -LiteralPath $texDir }
	Get-ChildItem $aside -File | Move-Item -Destination $shots -Force -ErrorAction Continue
	if (-not (Get-ChildItem $aside)) { Remove-Item -LiteralPath $aside }
}
$log = Get-Content (Join-Path $dst 'debug_h2.log')
$log | Select-String '^Vulkan validation'
$log | Select-String '^materials:' | Select-Object -First 3
$view = ''
for ($i = 0; $i -lt $log.Count; $i++) {
	if ($log[$i] -match '^T58_VIEW (\S+)') {
		$view = $Matches[1]
		for ($j = $i + 1; $j -lt $log.Count -and $j -lt $i + 12 -and $log[$j] -notmatch '^T58_'; $j++) {
			if ($log[$j] -match '^here:|albedo:') { "${view}: $($log[$j].Trim())" }
		}
	}
}
$i = ($log | Select-String '^T58_END').LineNumber
if ($i) { "problems:"; $log[$i..([Math]::Min($i + 6, $log.Count - 1))] }
