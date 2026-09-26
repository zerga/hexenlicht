param([string]$Pak, [string]$Map, [string]$Models)
# prints the bounds of brush submodels (*N) of maps/<Map>.bsp in a pak, and
# the info_player_start origins (Hexen II BSP 29: dmodel_t with 8 hulls, 80 bytes)
$bytes = [IO.File]::ReadAllBytes($Pak)
$dirofs = [BitConverter]::ToInt32($bytes, 4); $dirlen = [BitConverter]::ToInt32($bytes, 8)
for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
	$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
	if ($name -ne "maps/$Map.bsp") { continue }
	$pos = [BitConverter]::ToInt32($bytes, $o + 56)
	$entofs = [BitConverter]::ToInt32($bytes, $pos + 4); $entlen = [BitConverter]::ToInt32($bytes, $pos + 8)
	$modofs = [BitConverter]::ToInt32($bytes, $pos + 4 + 14 * 8)
	foreach ($m in ($Models -split "," | ForEach-Object { [int]$_ })) {
		$b = $pos + $modofs + $m * 80
		$v = 0..5 | ForEach-Object { [Math]::Round([BitConverter]::ToSingle($bytes, $b + 4 * $_)) }
		"*{0}: mins {1} {2} {3} maxs {4} {5} {6}" -f $m, $v[0], $v[1], $v[2], $v[3], $v[4], $v[5]
	}
	$ents = [Text.Encoding]::ASCII.GetString($bytes, $pos + $entofs, $entlen)
	[regex]::Matches($ents, '\{[^}]*\}') | ForEach-Object { $_.Value } | Where-Object { $_ -match '"classname"\s+"info_player_start"' } |
		ForEach-Object { if ($_ -match '"origin"\s+"([^"]*)"') { "info_player_start at " + $Matches[1] } }
}
