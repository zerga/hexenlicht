param([string]$Texconv = '', [string]$Data = '', [switch]$Run, [switch]$Portals, [switch]$Release, [switch]$Remove, [int]$Timeout = 600)

# The special materials' test set (story 5.5, docs/hexenlicht/TESTING.md,
# "Special materials"): generated files (no game data) under the material
# spec's names (docs/hexenlicht/MATERIALS.md) in data1\textures, and the
# test scripts spectest_a.cfg to _d (data1) and spectest_p.cfg (-portals):
#  - kinds: demo1's floor rtex429 chrome (roughness 0.01: a mirror), its
#    pedestal rtex426 glass, both with a light albedo (a mirror and glass
#    are tinted by it); the player (models/paladin.mdl_0) and the
#    gauntlet chrome; +1rune2's kind refused (not its animation's first
#    frame); at T55_SWAP_B rtex426.mat becomes "kind regular": the reload
#    reports the kind as changed, the next map load applies it;
#  - liquids: demo1's pool #rtex346 an albedo (cyan checker) and a normal
#    map leaning towards the image's top, the translucent #lowlight an
#    albedo (green checker);
#  - animated: +0rune1 to +4rune1 red, green, blue, yellow, magenta;
#  - lava: meso9's #lava000 a blue checker with .mat emissive 1; at
#    T55_SWAP_A emissive 2 (the reload: its lights twice as bright);
#  - the sky: sky001~6566 (demo1's and the Praevus maps') 512x256, its
#    front (left half) red with a checker of transparent squares, its back
#    blue; at T55_SWAP_B the same without alpha (the original's
#    transparency); sky000 (every sky000: egypt1's) a yellow front and a
#    green back without alpha, with a sky000_n.png the sky doesn't take;
#    sky001~4893 (meso's) a BC7 DDS the sky refuses.
# The images are TGAs made here, converted with Microsoft's texconv
# (DirectXTex, -Texconv, not in the repository) as 5.3's set is. Refuses to
# run if data1\textures has files it didn't write, and lists what it wrote
# in data1\spectest_files.txt; -Remove deletes those, the scripts and the
# sources (data1\spectest_src).
# -Run starts hl_run.ps1 -Cfg spectest_a.cfg (-Release) and swaps the files
# at T55_SWAP_A and T55_SWAP_B; -Run -Portals runs spectest_p.cfg with
# -portals (keep1's sky). special_check.ps1 checks the screenshots
# (data1\shots hexen00 to hexen18, portals\shots hexen00) and the log.
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$game = Join-Path $Data 'data1'
$tex = Join-Path $game 'textures'
$src = Join-Path $game 'spectest_src'
$manifest = Join-Path $game 'spectest_files.txt'
$scripts = 'spectest_a.cfg', 'spectest_a2.cfg', 'spectest_b.cfg', 'spectest_c.cfg', 'spectest_d.cfg', 'spectest_p.cfg'

if ($Remove) {
	if (Test-Path $manifest) {
		foreach ($f in Get-Content $manifest) { $p = Join-Path $game $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p } }
		Remove-Item -LiteralPath $manifest
	}
	foreach ($s in $scripts) { $p = Join-Path $game $s; if (Test-Path $p) { Remove-Item -LiteralPath $p } }
	if (Test-Path $src) { Remove-Item -LiteralPath $src -Recurse }
	foreach ($d in (Join-Path $tex 'models'), $tex) {
		if ((Test-Path $d) -and -not (Get-ChildItem -LiteralPath $d -Recurse -File)) { Remove-Item -LiteralPath $d -Recurse }
	}
	"removed the special materials' test set"
	return
}

if ($Run) {
	$log = Join-Path $Data 'debug_h2.log'
	$runArgs = @{ Cfg = 'spectest_a.cfg'; Timeout = $Timeout }
	if ($Release) { $runArgs.Release = $true }
	if ($Portals) {
		$runArgs.Cfg = 'spectest_p.cfg'
		$runArgs.Portals = $true
		& (Join-Path $PSScriptRoot 'hl_run.ps1') @runArgs
		return
	}
	$swap = {
		param($log, $game, $src)
		$deadline = (Get-Date).AddSeconds(900)
		$state = 0; $said = @()
		function Seen([string]$m) { (Test-Path $log) -and (Select-String -LiteralPath $log -Pattern $m -SimpleMatch -Quiet) }
		while ((Get-Date) -lt $deadline -and $state -lt 2) {
			Start-Sleep -Milliseconds 100
			if ($state -eq 0 -and (Seen 'T55_SWAP_A')) {
				Copy-Item -LiteralPath (Join-Path $src 'lava_emissive2.mat') -Destination (Join-Path $game 'textures\#lava000.mat') -Force
				$said += 'A: #lava000.mat emissive 2'; $state = 1
			}
			elseif ($state -eq 1 -and (Seen 'T55_SWAP_B')) {
				Copy-Item -LiteralPath (Join-Path $src 'sky_noalpha.png') -Destination (Join-Path $game 'textures\sky001~6566.png') -Force
				Set-Content -LiteralPath (Join-Path $game 'textures\rtex426.mat') -Value 'kind regular'
				$said += 'B: the sky without alpha, rtex426 regular'; $state = 2
			}
		}
		if ($state -lt 2) { $said += "stopped at step $state" }
		return ($said -join ', ')
	}
	if (Test-Path $log) { Remove-Item -LiteralPath $log }
	$job = Start-Job -ScriptBlock $swap -ArgumentList $log, $game, $src
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

if (-not ('SpecialTestSet' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO;
public static class SpecialTestSet {
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
	public static void Flat(string file, int r, int g, int b) { Tga(file, 16, 16, (x, y) => new[] { r, g, b, 255 }); }
	public static void Checker(string file, int w, int h, int s, int[] c0, int[] c1) {
		Tga(file, w, h, (x, y) => ((x / s + y / s) & 1) == 0 ? c0 : c1);
	}
	// a sky: the front (left half) of one color, transparent in a checker of s texels if holes; the back of another
	public static void Sky(string file, int w, int[] front, int[] back, bool holes, int s) {
		int h = w / 2;
		Tga(file, w, h, (x, y) => (x < h) ? new[] { front[0], front[1], front[2], (holes && ((x / s + y / s) & 1) == 1) ? 0 : 255 }
					  : new[] { back[0], back[1], back[2], 255 });
	}
}
'@
}

$red = 255, 0, 0; $blue = 0, 0, 255
[SpecialTestSet]::Sky((Join-Path $src 'sky_alpha.tga'), 512, $red, $blue, $true, 32)
[SpecialTestSet]::Sky((Join-Path $src 'sky_noalpha.tga'), 512, $red, $blue, $false, 32)
[SpecialTestSet]::Sky((Join-Path $src 'sky_egypt.tga'), 256, @(255, 255, 0), @(0, 255, 0), $false, 32)
[SpecialTestSet]::Checker((Join-Path $src 'pool.tga'), 64, 64, 8, @(0, 255, 255, 255), @(0, 60, 60, 255))
[SpecialTestSet]::Checker((Join-Path $src 'lowlight.tga'), 64, 64, 8, @(40, 255, 40, 255), @(0, 90, 0, 255))
[SpecialTestSet]::Checker((Join-Path $src 'lava.tga'), 64, 64, 8, @(40, 90, 255, 255), @(10, 20, 120, 255))
# the normal (0, 0.5, 0.866): towards the image's top (OpenGL's convention), XYZ as RGB x 0.5 + 0.5
[SpecialTestSet]::Flat((Join-Path $src 'normal_up.tga'), 128, 191, 238)
[SpecialTestSet]::Flat((Join-Path $src 'flat_n.tga'), 128, 128, 255)
# a light albedo: a mirror and glass are tinted by it (Quake II RTX's chrome and glass)
[SpecialTestSet]::Flat((Join-Path $src 'light.tga'), 230, 230, 230)
$runes = @(@('red', 255, 0, 0), @('green', 0, 255, 0), @('blue', 0, 0, 255), @('yellow', 255, 255, 0), @('magenta', 255, 0, 255))
foreach ($r in $runes) { [SpecialTestSet]::Flat((Join-Path $src "$($r[0]).tga"), $r[1], $r[2], $r[3]) }

$written = New-Object System.Collections.Generic.List[string]
function Convert-Image([string]$In, [string]$Out, [string[]]$Opt) {
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

Text 'textures\rtex429.mat' @('# a mirror: chrome below roughness 0.02', 'kind chrome', 'roughness 0.01')
Text 'textures\rtex426.mat' @('kind glass')
Png 'light.tga' 'textures\rtex429.png'
Png 'light.tga' 'textures\rtex426.png'
Text 'textures\models\paladin.mdl_0.mat' @('kind chrome', 'roughness 0.01')
Text 'textures\models\gauntlet.mdl_0.mat' @('kind chrome', 'roughness 0.01')
Text 'textures\+1rune2.mat' @('# refused: an animated texture''s kind is its first frame''s', 'kind chrome')
Png 'pool.tga' 'textures\#rtex346.png'
Png 'normal_up.tga' 'textures\#rtex346_n.png'
Png 'lowlight.tga' 'textures\#lowlight.png'
for ($i = 0; $i -lt $runes.Count; $i++) { Png "$($runes[$i][0]).tga" "textures\+$($i)rune1.png" }
Png 'lava.tga' 'textures\#lava000.png'
Text 'textures\#lava000.mat' @('emissive 1')
Png 'sky_alpha.tga' 'textures\sky001~6566.png'
Png 'sky_egypt.tga' 'textures\sky000.png'
Png 'flat_n.tga' 'textures\sky000_n.png'
Convert-Image 'sky_egypt.tga' 'textures\sky001~4893.dds' @('-f', 'BC7_UNORM', '-dx10', '-m', '1')
# the swaps' files, outside textures\ (-Run copies them in)
$o = & $Texconv -nologo -y -o $src -ft png -f R8G8B8A8_UNORM -m 1 (Join-Path $src 'sky_noalpha.tga') 2>&1
if ($LASTEXITCODE -ne 0) { throw "texconv sky_noalpha: $o" }
Set-Content -LiteralPath (Join-Path $src 'lava_emissive2.mat') -Value 'emissive 2'
Remove-Item -LiteralPath (Join-Path $src 'out') -Recurse -ErrorAction SilentlyContinue
Set-Content -LiteralPath $manifest -Value $written

# the scripts: waits on one line each (a frame per wait); passes: pt_reflect_refract
# (0: the primary surfaces in the G-buffer, 2: Quake II RTX's default)
function Waits([int]$n) { (@('wait') * $n) -join ';' }
function Shot([string]$view, [int]$passes) { "pt_reflect_refract $passes"; "r_debugview $view"; Waits 12; 'screenshot'; Waits 8 }
function SetPos([string]$pos) { 'pause'; "vk_setpos $pos"; Waits 10; 'pause'; Waits 5 }
$start = @((Waits 3), 'vid_vsync 0', 'host_framerate 0.02', 'color 0 0', 'flt_enable 0', 'viewsize 130', 'showpause 0',
	'crosshair 0', 'r_materials 1', 'r_skyalpha 0.67', 'r_sky_light 0')
$a = $start + @(
	'playerclass 1', 'map demo1', (Waits 150), 'notarget', 'noclip', (Waits 30), 'pause', (Waits 15),
	(Shot 1 0), (Shot 3 0), (Shot 1 2), (Shot 0 2),
	'chase_active 1', (Waits 10), (Shot 3 0), (Shot 0 2), 'chase_active 0', 'r_debugview 0',
	'echo T55_INSTANCES', 'vk_instances', 'vk_models check', 'vk_world',
	(SetPos '-1536 2226 -466 35 90'), (Shot 2 0), (Shot 8 0), (Shot 1 0),
	'pause', (Waits 10), 'pause', (Waits 5), (Shot 1 0), 'pause', (Waits 10), 'pause', (Waits 5), (Shot 1 0),
	'exec spectest_a2.cfg')	# still paused: a2 moves first
$a2 = @(
	(SetPos '474 -80 -330 35 90'), (Shot 1 0), (Shot 0 2),
	(SetPos '-885 -1828 214 -80 0'), (Shot 0 2), 'echo T55_SKY_A', 'r_sky_light 1', 'vk_sky', 'r_sky_light 0',
	'echo T55_PROBLEMS_A', 'vk_materials list', 'vk_materials problems', 'pause', 'exec spectest_b.cfg')
$b = @(
	'map meso9', (Waits 150), 'notarget', (Waits 30), 'pause', (Waits 15), (Shot 0 2), 'echo T55_LAVA_1', 'vk_lights',
	'echo T55_PROBLEMS_B', 'vk_materials problems', 'echo T55_SWAP_A', (Waits 300), 'r_reloadmaterials', (Waits 10),
	(Shot 0 2), 'echo T55_LAVA_2', 'vk_lights', 'pause', 'exec spectest_c.cfg')
$c = @(
	'map demo1', (Waits 150), 'notarget', 'noclip', (Waits 30), 'pause', (Waits 15), (SetPos '-885 -1828 214 -80 0'),
	'echo T55_SWAP_B', (Waits 300), 'echo T55_RELOAD_B', 'r_reloadmaterials', (Waits 10), (Shot 0 2),
	'echo T55_SKY_B', 'r_sky_light 1', 'vk_sky', 'r_sky_light 0', 'pause',
	'map demo1', (Waits 150), 'notarget', (Waits 30), 'pause', (Waits 15), (Shot 3 0), 'pause', 'exec spectest_d.cfg')
$d = @(
	'map egypt1', (Waits 150), 'notarget', 'noclip', (Waits 30), 'pause', (Waits 15), (SetPos '-386 130 214 -80 0'), (Shot 0 2),
	'echo T55_SKY_EGYPT', 'vk_sky', 'echo T55_PROBLEMS_D', 'vk_materials problems',
	'r_debugview 0', 'pt_reflect_refract 2', 'pause', 'vid_vsync 1', 'playerclass 1', 'toggleconsole', (Waits 10), 'quit')
$p = $start + @(
	'playerclass 1', 'map keep1', (Waits 150), 'notarget', 'noclip', (Waits 30), 'pause', (Waits 15), (SetPos '413 192 358 -80 0'),
	(Shot 0 2), 'echo T55_SKY_KEEP', 'vk_sky', 'vk_materials list', 'vk_materials problems',
	'r_debugview 0', 'pt_reflect_refract 2', 'pause', 'vid_vsync 1', 'playerclass 1', 'toggleconsole', (Waits 10), 'quit')
Set-Content -LiteralPath (Join-Path $game 'spectest_a.cfg') -Value $a
Set-Content -LiteralPath (Join-Path $game 'spectest_a2.cfg') -Value $a2
Set-Content -LiteralPath (Join-Path $game 'spectest_b.cfg') -Value $b
Set-Content -LiteralPath (Join-Path $game 'spectest_c.cfg') -Value $c
Set-Content -LiteralPath (Join-Path $game 'spectest_d.cfg') -Value $d
Set-Content -LiteralPath (Join-Path $game 'spectest_p.cfg') -Value $p
"wrote $($written.Count) files into data1\textures (listed in spectest_files.txt) and spectest_a.cfg to _d, _p"
