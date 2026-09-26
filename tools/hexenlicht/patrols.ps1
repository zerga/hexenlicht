param([string[]]$Paks, [double]$Radius = 1500)
# per map: monsters with a "target" (a path_corner patrol they walk from the start) within $Radius of info_player_start
foreach ($pak in $Paks) {
	$bytes = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4); $dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
		$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if ($name -notmatch '^maps/(.*)\.bsp$') { continue }
		$map = $Matches[1]
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$entofs = [BitConverter]::ToInt32($bytes, $pos + 4); $entlen = [BitConverter]::ToInt32($bytes, $pos + 8)
		$ents = [regex]::Matches([Text.Encoding]::ASCII.GetString($bytes, $pos + $entofs, $entlen), '\{[^}]*\}') | ForEach-Object { $_.Value }
		$start = $ents | Where-Object { $_ -match '"classname"\s+"info_player_start"' } | Select-Object -First 1
		if (-not $start -or $start -notmatch '"origin"\s+"([^"]*)"') { continue }
		$s = $Matches[1].Split(' ') | ForEach-Object { [double]$_ }
		$found = foreach ($e in $ents) {
			if ($e -notmatch '"classname"\s+"(monster_[^"]*)"') { continue }
			$cls = $Matches[1]
			if ($e -notmatch '"target"\s+"[^"]+"' -or $e -notmatch '"origin"\s+"([^"]*)"') { continue }
			$p = $Matches[1].Split(' ') | ForEach-Object { [double]$_ }
			$d = [Math]::Sqrt(($p[0]-$s[0])*($p[0]-$s[0]) + ($p[1]-$s[1])*($p[1]-$s[1]) + ($p[2]-$s[2])*($p[2]-$s[2]))
			if ($d -le $Radius) { "{0}@{1:F0}" -f $cls, $d }
		}
		if ($found) { "{0,-10} start {1}: {2}" -f $map, ($s -join ' '), ($found -join ', ') }
	}
}
