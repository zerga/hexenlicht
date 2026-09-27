param([string]$Exe, [string]$Bsp, [string]$Out)

# Runs a jsh2colour.exe built with jsh2color_colors.patch (each light's color
# printed, no .lit written) on every map in $Bsp with the options the tool's
# batch files (utils/jsh2color/data_win/colour*.bat) give it, the others
# with -extra and the built-in list; writes $Out\<map>.txt ("LC classname
# x y z r g b" lines) for light_colors_compare.ps1. See TESTING.md "Light
# colors".
$repo = Split-Path (Split-Path $PSScriptRoot)
$data = Join-Path $repo 'utils\jsh2color\data_win'
$opts = @{}
foreach ($bat in Get-ChildItem (Join-Path $data 'colour*.bat')) {
	foreach ($line in Get-Content $bat) {
		$w = -split $line
		if ($w.Count -lt 2 -or $w[0] -ne 'jsh2colour') { continue }
		$args2 = @($w[1..($w.Count - 2)] | Where-Object { $_ -ne '-threads' -and $_ -ne '-1' })
		for ($i = 0; $i -lt $args2.Count; $i++) {
			if ($args2[$i] -eq '-external') { $args2[$i + 1] = Join-Path $data $args2[$i + 1] }
		}
		$opts[$w[-1]] = $args2
	}
}
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($f in Get-ChildItem $Bsp -Filter *.bsp) {
	$map = $f.BaseName
	$a = @(if ($opts.ContainsKey($map)) { $opts[$map] } else { '-extra' })
	# one thread: the tool's threads add to the sums without a lock
	$text = & $Exe -threads 1 @a $f.FullName 2>&1
	$text | Set-Content (Join-Path $Out "$map.txt")
	$n = @($text | Where-Object { $_ -like 'LC *' }).Count
	"{0,-10} {1,-45} {2,4} lights{3}" -f $map, ($a -join ' ' -replace [regex]::Escape($data + '\'), ''), $n,
		$(if ($text -match 'no light color modifying data') { ' (none colored: no .lit)' } else { '' })
}
exit 0
