param([string]$Texconv = '', [string]$Source = '', [string]$Out = '')

# Converts material files as authored into the shipping formats (story 5.6,
# docs/hexenlicht/AUTHORING.md "7. Ship it", MATERIALS.md "Formats and
# lookup"): every PNG and TGA under -Source (a textures folder: the spec's
# names, subfolders such as models\ kept) into -Out with Microsoft's texconv
# (DirectXTex, -Texconv, not in the repository), with a full mip chain:
#  - normal maps (_n) as BC5_UNORM;
#  - albedo, emissive (_e) and _orm as BC7_UNORM (an albedo's alpha kept);
#  - _r, _m and the sky (sky...) copied as they are: the engine packs _r and
#    _m into one texture at load and splits the sky's layers, which a
#    compressed file can't be;
#  - .mat files copied.
# The 8-bit values are kept (--ignore-srgb: a PNG's gamma or sRGB chunk
# doesn't convert them; the engine reads every format as UNORM and decodes
# colors itself), so a DDS shows what its PNG shows but for BC7's and BC5's
# small errors. BC needs sizes in multiples of 4 (4x an original always is).
# The engine reads a PNG before a DDS of the same name: ship one or the
# other.
$ErrorActionPreference = 'Stop'
if (-not $Texconv -or -not $Source -or -not $Out) { throw "pack_dds.ps1 -Texconv <texconv.exe> -Source <folder> -Out <folder>" }
$Source = (Resolve-Path -LiteralPath $Source).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
if ($Out -eq $Source) { throw "-Out must be another folder than -Source" }
$t0 = Get-Date
$counts = @{ BC7 = 0; BC5 = 0; copied = 0 }

$groups = @{}
foreach ($f in Get-ChildItem -LiteralPath $Source -Recurse -File) {
	$rel = $f.FullName.Substring($Source.Length).TrimStart('\')
	$dir = Split-Path $rel
	$dest = if ($dir) { Join-Path $Out $dir } else { $Out }
	New-Item -ItemType Directory -Force $dest | Out-Null
	$base = [IO.Path]::GetFileNameWithoutExtension($f.Name).ToLowerInvariant()
	$ext = $f.Extension.ToLowerInvariant()
	$format = $null
	if ($ext -in '.png', '.tga') {
		if ($base -match '_n$') { $format = 'BC5_UNORM' }
		elseif ($base -match '_(r|m)$' -or (-not $dir -and $base -match '^sky')) { $format = $null }
		else { $format = 'BC7_UNORM' }
	}
	if (-not $format) {
		Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
		$counts.copied++
		continue
	}
	$key = "$format|$dest"
	if (-not $groups.ContainsKey($key)) { $groups[$key] = New-Object System.Collections.Generic.List[string] }
	$groups[$key].Add($f.FullName)
}
foreach ($key in $groups.Keys) {
	$format, $dest = $key -split '\|', 2
	$files = $groups[$key]
	for ($i = 0; $i -lt $files.Count; $i += 32) {
		$batch = $files[$i..([Math]::Min($i + 31, $files.Count - 1))]
		$o = & $Texconv -nologo -y -o $dest -ft dds -dx10 -f $format -m 0 --ignore-srgb @batch 2>&1
		if ($LASTEXITCODE -ne 0) { throw "texconv $format`: $o" }
	}
	$counts[$format.Substring(0, 3)] += $files.Count
}
$mb = (Get-ChildItem -LiteralPath $Out -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
"pack_dds: {0} BC7, {1} BC5, {2} copied, into {3} ({4:N1} MB) in {5:N1} s" -f $counts.BC7, $counts.BC5, $counts.copied, $Out, $mb, ((Get-Date) - $t0).TotalSeconds
