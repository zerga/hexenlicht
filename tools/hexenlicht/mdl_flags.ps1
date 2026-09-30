param([string[]]$Paks, [switch]$Trails)
# lists the .mdl files with EF_ROTATE, EF_FACE_VIEW or a transparency flag;
# -Trails: the files with a trail flag instead, by the flag cl_main.c's
# CL_RelinkEntities uses (its else-if chain: the first flag set wins; 6.1)
$trail_order = @(
	@(0x4, 'GIB'), @(0x20, 'ZOMGIB'), @(0x800000, 'BLOODSHOT'), @(0x10, 'TRACER'),
	@(0x40, 'TRACER2'), @(0x1, 'ROCKET'), @(0x100, 'FIREBALL'), @(0x400000, 'ACIDBALL'),
	@(0x200, 'ICE'), @(0x800, 'SPIT'), @(0x2000, 'SPELL'), @(0x2, 'GRENADE'),
	@(0x80, 'TRACER3'), @(0x20000, 'VORP_MISSILE'), @(0x40000, 'SET_STAFF'),
	@(0x80000, 'MAGICMISSILE'), @(0x100000, 'BONESHARD'), @(0x200000, 'SCARAB'))
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
		if ($Trails) {
			foreach ($t in $trail_order) { if ($flags -band $t[0]) { $f += $t[1] } }
			if ($f.Count -gt 1) { $f = @($f[0] + ' (also ' + ($f[1..($f.Count - 1)] -join ' ') + ')') }
		} else {
			if ($flags -band 8) { $f += 'ROTATE' }
			if ($flags -band 0x10000) { $f += 'FACE_VIEW' }
			if ($flags -band 0x1000) { $f += 'TRANSPARENT' }
			if ($flags -band 0x4000) { $f += 'HOLEY' }
			if ($flags -band 0x8000) { $f += 'SPECIAL_TRANS' }
		}
		if ($f.Count) { [pscustomobject]@{ pak = [IO.Path]::GetFileName($pak); name = $name; flags = ($f -join ' ') } }
	}
}
$rows | Group-Object flags | ForEach-Object { "{0} ({1}): {2}" -f $_.Name, $_.Count, (($_.Group | ForEach-Object { $_.name -replace '^models/','' }) -join ', ') }
