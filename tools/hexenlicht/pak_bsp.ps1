param([string[]]$Paks, [string]$Out)

# Extracts the maps (maps/*.bsp) of the given paks into $Out, e.g. for
# utils/jsh2color (jsh2color_colors.ps1). They are the game's data: keep them
# out of the repository and delete them afterwards.
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($pak in $Paks) {
	$bytes = [System.IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4)
	$dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($e = 0; $e -lt $dirlen / 64; $e++) {
		$o = $dirofs + $e * 64
		$name = [System.Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if ($name -notmatch '^maps/.*\.bsp$') { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$len = [BitConverter]::ToInt32($bytes, $o + 60)
		$seg = New-Object byte[] $len
		[Array]::Copy($bytes, $pos, $seg, 0, $len)
		[System.IO.File]::WriteAllBytes((Join-Path $Out ($name -replace '^maps/', '')), $seg)
	}
}
"{0} maps in {1}" -f (Get-ChildItem $Out -Filter *.bsp).Count, $Out
