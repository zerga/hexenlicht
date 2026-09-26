param([string[]]$Paks)
# lists the .mdl files with EF_ROTATE, EF_FACE_VIEW or a transparency flag
$rows = foreach ($pak in $Paks) {
	$bytes = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4)
	$dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
		$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if (-not $name.EndsWith('.mdl')) { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$flags = [BitConverter]::ToInt32($bytes, $pos + 8 + 12 + 12 + 4 + 12 + 28)
		$f = @()
		if ($flags -band 8) { $f += 'ROTATE' }
		if ($flags -band 0x10000) { $f += 'FACE_VIEW' }
		if ($flags -band 0x1000) { $f += 'TRANSPARENT' }
		if ($flags -band 0x4000) { $f += 'HOLEY' }
		if ($flags -band 0x8000) { $f += 'SPECIAL_TRANS' }
		if ($f.Count) { [pscustomobject]@{ pak = [IO.Path]::GetFileName($pak); name = $name; flags = ($f -join ' ') } }
	}
}
$rows | Group-Object flags | ForEach-Object { "{0} ({1}): {2}" -f $_.Name, $_.Count, (($_.Group | ForEach-Object { $_.name -replace '^models/','' }) -join ', ') }
