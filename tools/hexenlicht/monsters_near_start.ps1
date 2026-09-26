param([string[]]$Paks, [double]$Radius = 700)
# per map: monsters within $Radius of info_player_start (spawnflags ignored)
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
		$start = $null; $mons = @()
		foreach ($m in [regex]::Matches($text, '\{[^{}]*\}')) {
			$ent = $m.Value
			if ($ent -notmatch '"classname"\s+"([^"]*)"') { continue }
			$cls = $Matches[1]
			if ($ent -notmatch '"origin"\s+"([^"]*)"') { continue }
			$org = $Matches[1] -split '\s+' | ForEach-Object { [double]$_ }
			if ($cls -eq 'info_player_start' -and -not $start) { $start = $org }
			elseif ($cls -like 'monster_*') { $mons += ,@($cls, $org) }
		}
		if (-not $start) { continue }
		$near = @()
		foreach ($mo in $mons) {
			$d = [math]::Sqrt([math]::Pow($mo[1][0]-$start[0],2) + [math]::Pow($mo[1][1]-$start[1],2) + [math]::Pow($mo[1][2]-$start[2],2))
			if ($d -lt $Radius) { $near += ('{0}@{1:N0}' -f $mo[0], $d) }
		}
		if ($near.Count) { '{0,-10} start {1}: {2}' -f ($name -replace '^maps/|\.bsp$',''), ($start -join ' '), ($near -join ', ') }
	}
}
