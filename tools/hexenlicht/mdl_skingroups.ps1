param([string[]]$Paks)
# lists .mdl files whose skins include a skin group (animated skin); IDPO and RAPO headers are 84 bytes before the skins
foreach ($pak in $Paks) {
	$bytes = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4)
	$dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
		$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if (-not $name.EndsWith('.mdl')) { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$ident = [Text.Encoding]::ASCII.GetString($bytes, $pos, 4)
		$h = $pos + 48
		$numskins = [BitConverter]::ToInt32($bytes, $h)
		$w = [BitConverter]::ToInt32($bytes, $h + 4)
		$hh = [BitConverter]::ToInt32($bytes, $h + 8)
		$skin = $pos + $(if ($ident -eq 'RAPO') { 88 } else { 84 })
		for ($i = 0; $i -lt $numskins; $i++) {
			$type = [BitConverter]::ToInt32($bytes, $skin)
			if ($type -eq 0) { $skin += 4 + $w * $hh }
			else {
				$n = [BitConverter]::ToInt32($bytes, $skin + 4)
				"{0} {1} skin {2}: group of {3}" -f [IO.Path]::GetFileName($pak), $name, $i, $n
				$skin += 8 + 4 * $n + $n * $w * $hh
			}
		}
	}
}
