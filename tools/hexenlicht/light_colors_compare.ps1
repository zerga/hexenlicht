param([string]$Log, [string]$Ref, [string[]]$Maps)

# Compares each map's "vk_lights colors" lines in a -condebug log (the maps
# loaded in the order of $Maps, one "vk_lights colors" each) with
# jsh2color_colors.ps1's $Ref\<map>.txt: every map light must have the
# tool's color at its origin (the tool also lists the lights Hexenlicht
# drops inside solid). See TESTING.md "Light colors".
$blocks = @(); $cur = @()
foreach ($line in Get-Content $Log) {
	if ($line -match ' -> ') { $w = -split $line; $cur += ($w[0..6] -join ' ') }
	elseif ($line -like '  colors:*') { $blocks += , @{ lines = $cur; summary = $line.Trim() }; $cur = @() }
}
if ($blocks.Count -ne $Maps.Count) { "the log has {0} maps' colors, not {1}" -f $blocks.Count, $Maps.Count; exit 1 }
$bad = 0; $total = 0
for ($i = 0; $i -lt $Maps.Count; $i++) {
	$refFile = Join-Path $Ref "$($Maps[$i]).txt"
	$refText = if (Test-Path $refFile) { @(Get-Content $refFile) } else { @() }
	$refLines = @($refText | Where-Object { $_ -like 'LC *' } | ForEach-Object { $_.Substring(3) })
	$left = New-Object System.Collections.Hashtable ([StringComparer]::Ordinal)
	foreach ($r in $refLines) { $left[$r] = 1 + [int]$left[$r] }
	$miss = 0
	if ($refLines.Count -eq 0) {
		# the tool colored none (no .lit): Hexenlicht's lights are white; no
		# lines for another reason (no file, the tool failed): all count
		$none = ($refText -match 'no light color modifying data') -and ($blocks[$i].summary -like '*colors none*')
		if (-not $none) { $miss = $blocks[$i].lines.Count }
	} else {
		foreach ($l in $blocks[$i].lines) { if ([int]$left[$l] -gt 0) { $left[$l]-- } else { $miss++ } }
	}
	$total += $blocks[$i].lines.Count; $bad += $miss
	"{0,-10} {1,4} lights, {2} not as the tool's ({3} in its list)" -f $Maps[$i], $blocks[$i].lines.Count, $miss, $refLines.Count
}
"{0} lights, {1} not as the tool's" -f $total, $bad
exit [int]($bad -gt 0)
