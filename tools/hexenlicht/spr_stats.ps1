param([string[]]$Paks)
# lists the .spr files: orientation type, frames (groups), largest frame size
$types = @('VP_PARALLEL_UPRIGHT', 'FACING_UPRIGHT', 'VP_PARALLEL', 'ORIENTED', 'VP_PARALLEL_ORIENTED')
foreach ($pak in $Paks) {
	$bytes = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4)
	$dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
		$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if (-not $name.EndsWith('.spr')) { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$type = [BitConverter]::ToInt32($bytes, $pos + 8)
		$numframes = [BitConverter]::ToInt32($bytes, $pos + 24)
		$p = $pos + 36
		$groups = 0; $maxw = 0; $maxh = 0; $sub = 0
		for ($f = 0; $f -lt $numframes; $f++) {
			$ft = [BitConverter]::ToInt32($bytes, $p); $p += 4
			$n = 1
			if ($ft -ne 0) { $groups++; $n = [BitConverter]::ToInt32($bytes, $p); $p += 4 + 4 * $n }
			for ($k = 0; $k -lt $n; $k++) {
				$w = [BitConverter]::ToInt32($bytes, $p + 8); $h = [BitConverter]::ToInt32($bytes, $p + 12)
				if ($w -gt $maxw) { $maxw = $w }; if ($h -gt $maxh) { $maxh = $h }
				$p += 16 + $w * $h; $sub++
			}
		}
		"{0,-10} {1,-28} {2,-21} {3,3} frames ({4} groups, {5} images) max {6}x{7}" -f [IO.Path]::GetFileName($pak), $name, $types[$type], $numframes, $groups, $sub, $maxw, $maxh
	}
}
