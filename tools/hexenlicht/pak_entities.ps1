param([string[]]$Paks, [string]$Pattern = 'func_rotating|func_train')

# Lists, per map in the given paks, the entities whose classname matches
# $Pattern, with their targetname (empty = starts on its own) and origin.
foreach ($pak in $Paks) {
	$bytes = [System.IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4)
	$dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($e = 0; $e -lt $dirlen / 64; $e++) {
		$o = $dirofs + $e * 64
		$name = [System.Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if ($name -notmatch '^maps/.*\.bsp$') { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$entofs = [BitConverter]::ToInt32($bytes, $pos + 4)
		$entlen = [BitConverter]::ToInt32($bytes, $pos + 8)
		$text = [System.Text.Encoding]::ASCII.GetString($bytes, $pos + $entofs, $entlen)
		foreach ($m in [regex]::Matches($text, '\{[^{}]*\}')) {
			$ent = $m.Value
			if ($ent -match '"classname"\s+"(' + $Pattern + ')"') {
				$cls = $Matches[1]
				$tn = if ($ent -match '"targetname"\s+"([^"]*)"') { $Matches[1] } else { '' }
				$model = if ($ent -match '"model"\s+"([^"]*)"') { $Matches[1] } else { '' }
				$org = if ($ent -match '"origin"\s+"([^"]*)"') { $Matches[1] } else { '' }
				"{0,-12} {1,-16} model {2,-5} targetname '{3}' origin '{4}'" -f ($name -replace '^maps/|\.bsp$',''), $cls, $model, $tn, $org
			}
		}
	}
}
