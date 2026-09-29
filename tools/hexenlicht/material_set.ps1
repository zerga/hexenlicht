param([string]$Texconv = '', [string]$Data = '', [switch]$Run, [switch]$Release, [switch]$Remove, [int]$Timeout = 400)

# The material system's test set (story 5.3, docs/hexenlicht/TESTING.md,
# "Materials"): generated images (no game data) under the material spec's
# names (docs/hexenlicht/MATERIALS.md) for textures at demo1's start and
# meso9's lava, the view weapons and the player, in data1\textures, and the
# test scripts mattest_a.cfg (to _c, with _b2) in data1:
#  - demo1's world: an albedo (rtex022, with a BC5 DDS normal map), normal
#    maps tilted 30 degrees towards the image's top (rtex021 PNG, rtex022
#    BC5: the shading normal must lean up the wall), _r alone (rtex021),
#    _orm (rtex040), _m alone with a .mat metallic factor (rtex429), .mat
#    values and bad lines (rtex388), the ~<crc> name against the plain one
#    and one with another CRC (rtex430~6995 red, rtex430 blue, rtex430~ffff
#    green; rtex430_r under the plain name), an _e with a .mat emissive
#    (rtex426), a .mat emissive without _e (rtex028), files the set must
#    refuse (rtex038_r.dds: _r only as PNG or TGA; rtex013_n.dds: BC7 isn't
#    for normal maps);
#  - skins: the Paladin's gauntlet (models/gauntlet.mdl_0), the Crusader's
#    ice mace, a masked model (EF_HOLEY: models/icestaff.mdl_0, an albedo
#    without alpha, then one whose left half is transparent: the handle
#    goes), the Paladin as the player (models/paladin.mdl_0: shown with the
#    colors 0 0, not with 4 4), the light models' flames (flame1, flame2,
#    cflmtrch: a green _e, on castle4's torch);
#  - a sprite: the ice mace's hit (models/icehit.spr_0 to _5), magenta at
#    twice the frames' size without alpha (the original's coverage);
#  - meso9's lava (#lava000): a blue albedo, which is also what emits.
# The images are TGAs made here, converted with Microsoft's texconv
# (DirectXTex, -Texconv, not in the repository) to PNG and DDS as 5.2's set
# is. Refuses to run if data1\textures has files it didn't write (a texture
# pack), and lists what it wrote in data1\mattest_files.txt; -Remove deletes
# those, the scripts and the sources (data1\mattest_src).
# -Run starts hl_run.ps1 -Cfg mattest_a.cfg (-Release) and, when the game
# prints T53_SWAP, swaps in the ice mace's skin with holes and deletes
# rtex430~6995.png, for the script's r_reloadmaterials (hot reload: the next
# shots show the holes and rtex430's plain blue); at T53_LOCK it writes half
# of a new rtex388~6280.png and holds it open without sharing (a file an
# editor is still writing: the reload must refuse it, not end the game), at
# T53_UNLOCK the rest (the next reload shows it yellow). material_check.ps1
# checks the screenshots (data1\shots, hexen00 to hexen22).
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$tex = Join-Path $game 'textures'
$src = Join-Path $game 'mattest_src'
$manifest = Join-Path $game 'mattest_files.txt'
$scripts = 'mattest_a.cfg', 'mattest_b.cfg', 'mattest_b2.cfg', 'mattest_c.cfg'

if ($Remove) {
	if (Test-Path $manifest) {
		foreach ($f in Get-Content $manifest) { $p = Join-Path $game $f; if (Test-Path $p) { Remove-Item -LiteralPath $p } }
		Remove-Item -LiteralPath $manifest
	}
	foreach ($s in $scripts) { $p = Join-Path $game $s; if (Test-Path $p) { Remove-Item -LiteralPath $p } }
	if (Test-Path $src) { Remove-Item -LiteralPath $src -Recurse }
	# the folders the set made, if empty now
	foreach ($d in (Join-Path $tex 'models'), $tex) {
		if ((Test-Path $d) -and -not (Get-ChildItem -LiteralPath $d -Recurse -File)) { Remove-Item -LiteralPath $d -Recurse }
	}
	"removed the material test set"
	return
}

if ($Run) {
	$log = Join-Path $Data 'debug_h2.log'
	$swap = {
		param($log, $game, $src)
		$deadline = (Get-Date).AddSeconds(600)
		$state = 0; $fs = $null; $said = @()
		function Seen([string]$m) { (Test-Path $log) -and (Select-String -LiteralPath $log -Pattern $m -SimpleMatch -Quiet) }
		while ((Get-Date) -lt $deadline -and $state -lt 3) {
			Start-Sleep -Milliseconds 100
			if ($state -eq 0 -and (Seen 'T53_SWAP')) {
				Copy-Item -LiteralPath (Join-Path $src 'icestaff_half.png') -Destination (Join-Path $game 'textures\models\icestaff.mdl_0.png') -Force
				Remove-Item -LiteralPath (Join-Path $game 'textures\rtex430~6995.png')
				$said += 'swapped'; $state = 1
			}
			elseif ($state -eq 1 -and (Seen 'T53_LOCK')) {
				# a file an editor is still writing: half of it, held open without sharing
				$bytes = [IO.File]::ReadAllBytes((Join-Path $src 'yellow.png'))
				$fs = [IO.File]::Open((Join-Path $game 'textures\rtex388~6280.png'), 'Create', 'Write', 'None')
				$fs.Write($bytes, 0, [int]($bytes.Length / 2)); $fs.Flush()
				$script:rest = $bytes
				$said += 'locked'; $state = 2
			}
			elseif ($state -eq 2 -and (Seen 'T53_UNLOCK')) {
				$fs.Write($script:rest, [int]($script:rest.Length / 2), $script:rest.Length - [int]($script:rest.Length / 2)); $fs.Close(); $fs = $null
				$said += 'unlocked'; $state = 3
			}
		}
		if ($fs) { $fs.Close() }
		if ($state -lt 3) { $said += "stopped at step $state" }
		return ($said -join ', ')
	}
	if (Test-Path $log) { Remove-Item -LiteralPath $log }
	$job = Start-Job -ScriptBlock $swap -ArgumentList $log, $game, $src
	$runArgs = @{ Cfg = 'mattest_a.cfg'; Timeout = $Timeout }
	if ($Release) { $runArgs.Release = $true }
	& (Join-Path $PSScriptRoot 'hl_run.ps1') @runArgs
	Wait-Job $job -Timeout 5 | Out-Null
	Receive-Job $job; Remove-Job $job -Force
	return
}

if (-not $Texconv) { throw "-Texconv <texconv.exe> is needed to make the set" }
if (Test-Path $tex) {
	$ours = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }
	$other = Get-ChildItem -LiteralPath $tex -Recurse -File | Where-Object { $ours -notcontains $_.FullName.Substring($game.Length + 1) }
	if ($other) { throw "data1\textures has other files (a texture pack?): $($other[0].FullName) ..." }
}
New-Item -ItemType Directory -Force $src, $tex, (Join-Path $tex 'models') | Out-Null

if (-not ('MaterialTestSet' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class MaterialTestSet {
	// a 32-bit TGA, rows top first
	public static void Tga(string file, int w, int h, Func<int,int,int[]> px) {
		var b = new byte[18 + w * h * 4];
		b[2] = 2; b[12] = (byte)w; b[13] = (byte)(w >> 8); b[14] = (byte)h; b[15] = (byte)(h >> 8); b[16] = 32; b[17] = 0x28;
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			var c = px(x, y); int o = 18 + (y * w + x) * 4;
			b[o] = (byte)c[2]; b[o + 1] = (byte)c[1]; b[o + 2] = (byte)c[0]; b[o + 3] = (byte)c[3];
		}
		File.WriteAllBytes(file, b);
	}
	public static void Solid(string file, int w, int h, int r, int g, int b, int a) { Tga(file, w, h, (x, y) => new[] { r, g, b, a }); }
	public static void Flat(string file, int r, int g, int b, int a) { Solid(file, 16, 16, r, g, b, a); }
	// two colors in squares of s texels; top: the first 16 rows white (the image's top)
	public static void Checker(string file, int w, int h, int s, int[] c0, int[] c1, bool top) {
		Tga(file, w, h, (x, y) => (top && y < 16) ? new[] { 255, 255, 255, 255 } : (((x / s + y / s) & 1) == 0 ? c0 : c1));
	}
	// a checker whose left half is transparent
	public static void Half(string file, int w, int h, int s, int[] c0, int[] c1) {
		Tga(file, w, h, (x, y) => {
			var c = ((x / s + y / s) & 1) == 0 ? c0 : c1;
			return new[] { c[0], c[1], c[2], (x < w / 2) ? 0 : 255 };
		});
	}
}
'@
}

# the sources
$orange = 230, 120, 40, 255; $blue = 30, 40, 140, 255
[MaterialTestSet]::Checker((Join-Path $src 'albedo.tga'), 256, 256, 32, $orange, $blue, $true)
# the normal (0, 0.5, 0.866): towards the image's top (OpenGL's convention), XYZ as RGB x 0.5 + 0.5
[MaterialTestSet]::Flat((Join-Path $src 'normal_up.tga'), 128, 191, 238, 255)
[MaterialTestSet]::Flat((Join-Path $src 'grey128.tga'), 128, 128, 128, 255)
[MaterialTestSet]::Flat((Join-Path $src 'grey255.tga'), 255, 255, 255, 255)
[MaterialTestSet]::Flat((Join-Path $src 'grey77.tga'), 77, 77, 77, 255)
[MaterialTestSet]::Flat((Join-Path $src 'orm.tga'), 255, 64, 255, 255)
[MaterialTestSet]::Flat((Join-Path $src 'red.tga'), 255, 0, 0, 255)
[MaterialTestSet]::Flat((Join-Path $src 'blue.tga'), 0, 0, 255, 255)
[MaterialTestSet]::Flat((Join-Path $src 'green.tga'), 0, 255, 0, 255)
[MaterialTestSet]::Flat((Join-Path $src 'yellow.tga'), 255, 220, 60, 255)
[MaterialTestSet]::Checker((Join-Path $src 'gauntlet.tga'), 128, 128, 16, @(40, 220, 40, 255), @(220, 40, 220, 255), $false)
[MaterialTestSet]::Checker((Join-Path $src 'icestaff.tga'), 128, 128, 16, @(0, 255, 255, 255), @(0, 0, 0, 255), $false)
[MaterialTestSet]::Half((Join-Path $src 'icestaff_half.tga'), 128, 128, 16, @(0, 255, 255, 255), @(0, 0, 0, 255))
[MaterialTestSet]::Checker((Join-Path $src 'paladin.tga'), 128, 128, 16, @(255, 230, 0, 255), @(120, 0, 200, 255), $false)
[MaterialTestSet]::Checker((Join-Path $src 'lava.tga'), 64, 64, 8, @(40, 90, 255, 255), @(10, 20, 120, 255), $false)
[MaterialTestSet]::Solid((Join-Path $src 'magenta.tga'), 96, 160, 255, 0, 255, 255)

$written = New-Object System.Collections.Generic.List[string]
function Convert-Image([string]$In, [string]$Out, [string[]]$Opt) {
	$dir = Split-Path (Join-Path $game $Out)
	$tmp = Join-Path $src 'out'
	New-Item -ItemType Directory -Force $tmp | Out-Null
	$o = & $Texconv -nologo -y -o $tmp @Opt (Join-Path $src $In) 2>&1
	if ($LASTEXITCODE -ne 0) { throw "texconv $Opt ${In}: $o" }
	$made = Get-ChildItem -LiteralPath $tmp -File | Select-Object -First 1
	Move-Item -LiteralPath $made.FullName -Destination (Join-Path $game $Out) -Force
	$written.Add($Out)
}
function Png([string]$In, [string]$Out) { Convert-Image $In $Out @('-ft', 'png', '-f', 'R8G8B8A8_UNORM', '-m', '1') }
function Text([string]$Out, [string[]]$Lines) { Set-Content -LiteralPath (Join-Path $game $Out) -Value $Lines; $written.Add($Out) }

Png 'albedo.tga' 'textures\rtex022.png'
Convert-Image 'normal_up.tga' 'textures\rtex022_n.dds' @('-f', 'BC5_UNORM', '-dx10')
Png 'normal_up.tga' 'textures\rtex021_n.png'
Png 'grey128.tga' 'textures\rtex021_r.png'
Png 'orm.tga' 'textures\rtex040_orm.png'
Png 'grey255.tga' 'textures\rtex429_m.png'
Text 'textures\rtex429.mat' @('# the metallic map (255) times 0.5', 'metallic 0.5')
Text 'textures\rtex388.mat' @('# values without maps', 'roughness 0.3', 'metallic 0.7', 'specular 0.5  # a comment after a value',
	'# problems: an unknown key, a value out of range, 5.5''s kind, no value', 'foo 1', 'roughness 2', 'kind chrome', 'specular')
Png 'red.tga' 'textures\rtex430~6995.png'
Png 'blue.tga' 'textures\rtex430.png'
Png 'green.tga' 'textures\rtex430~ffff.png'
Png 'grey77.tga' 'textures\rtex430_r.png'
Png 'yellow.tga' 'textures\rtex426_e.png'
Text 'textures\rtex426.mat' @('emissive 0.5')
Text 'textures\rtex028.mat' @('emissive 1')
Convert-Image 'grey128.tga' 'textures\rtex038_r.dds' @('-f', 'R8G8B8A8_UNORM', '-dx10', '-m', '1')
Convert-Image 'normal_up.tga' 'textures\rtex013_n.dds' @('-f', 'BC7_UNORM', '-dx10')
Png 'gauntlet.tga' 'textures\models\gauntlet.mdl_0.png'
Png 'icestaff.tga' 'textures\models\icestaff.mdl_0.png'
Png 'paladin.tga' 'textures\models\paladin.mdl_0.png'
Png 'lava.tga' 'textures\#lava000.png'
foreach ($m in 'flame1', 'flame2', 'cflmtrch') { Png 'green.tga' "textures\models\$m.mdl_0_e.png" }
foreach ($i in 0..5) { Png 'magenta.tga' "textures\models\icehit.spr_$i.png" }
# the hot reload's replacements, outside textures\ (-Run writes them in)
$o = & $Texconv -nologo -y -o $src -ft png -f R8G8B8A8_UNORM -m 1 (Join-Path $src 'icestaff_half.tga') (Join-Path $src 'yellow.tga') 2>&1
if ($LASTEXITCODE -ne 0) { throw "texconv icestaff_half, yellow: $o" }
$written.Add('textures\rtex388~6280.png')	# made by -Run: -Remove deletes it
Remove-Item -LiteralPath (Join-Path $src 'out') -Recurse -ErrorAction SilentlyContinue
Set-Content -LiteralPath $manifest -Value $written

# the scripts: waits on one line each (a frame per wait)
function Waits([int]$n) { (@('wait') * $n) -join ';' }
function Shot([string]$view) { "r_debugview $view"; Waits 12; 'screenshot'; Waits 8 }
$a = @(
	(Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'playerclass 1', 'color 0 0', 'flt_enable 0', 'viewsize 130', 'showpause 0',
	'crosshair 0', 'r_materials 1', 'map demo1', (Waits 150), 'notarget', (Waits 30), 'pause', (Waits 15),
	(Shot 1), (Shot 2), (Shot 8), (Shot 10), (Shot 0),
	'r_materials 0', (Waits 5), (Shot 1), (Shot 0), 'r_materials 1', (Waits 5),
	'chase_active 1', (Waits 10), (Shot 1), 'color 4 4', (Waits 20), (Shot 1), 'color 0 0', 'chase_active 0', 'r_debugview 0',
	'vk_materials', 'vk_materials list', 'vk_materials problems', 'r_reloadmaterials', 'pause', (Waits 5), 'exec mattest_b.cfg')
$b = @(
	'playerclass 2', 'map demo1', (Waits 150), 'notarget', 'impulse 9', (Waits 150), 'impulse 2', (Waits 100), 'pause', (Waits 15),
	(Shot 1), (Shot 0), 'echo T53_SWAP', (Waits 300), 'r_reloadmaterials', (Waits 10), (Shot 1), (Shot 0),
	'vk_materials list', 'vk_models check', 'pause', (Waits 5),
	# the ice mace's hits (sprites) on the pedestal, unpaused
	'r_debugview 1', '+attack', (Waits 25), 'screenshot', (Waits 6), 'screenshot', (Waits 6), 'screenshot', (Waits 4), '-attack',
	'r_debugview 0', (Waits 20), 'pause', (Waits 5), 'exec mattest_b2.cfg')
$b2 = @(
	# a file being written (refused, not the end of the game), then written
	'echo T53_LOCK', (Waits 300), 'r_reloadmaterials', (Waits 10), (Shot 1), 'vk_materials problems',
	'echo T53_UNLOCK', (Waits 300), 'r_reloadmaterials', (Waits 10), (Shot 1), 'r_debugview 0',
	'pause', (Waits 5), 'exec mattest_c.cfg')
$c = @(
	'playerclass 1', 'map meso9', (Waits 150), 'notarget', (Waits 30), 'pause', (Waits 15), (Shot 0), (Shot 1), 'r_debugview 0',
	'vk_materials', 'pause', (Waits 5),
	'map castle4', (Waits 150), 'notarget', (Waits 30), 'pause', (Waits 15), (Shot 0), 'r_materials 0', (Waits 5), (Shot 0),
	'r_materials 1', 'r_emissive_models 0', (Waits 5), (Shot 0), 'r_emissive_models 1',
	'vk_lights', 'pause', 'vid_vsync 1', 'playerclass 1', 'toggleconsole', (Waits 10), 'quit')
Set-Content -LiteralPath (Join-Path $game 'mattest_a.cfg') -Value $a
Set-Content -LiteralPath (Join-Path $game 'mattest_b.cfg') -Value $b
Set-Content -LiteralPath (Join-Path $game 'mattest_b2.cfg') -Value $b2
Set-Content -LiteralPath (Join-Path $game 'mattest_c.cfg') -Value $c
"wrote $($written.Count) files into data1\textures (listed in mattest_files.txt) and mattest_a.cfg to _c"
