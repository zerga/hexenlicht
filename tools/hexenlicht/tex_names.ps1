param([string[]]$Paks, [switch]$All)

# The world textures of the given paks' maps under the material spec's names
# (docs/hexenlicht/MATERIALS.md, story 5.1): per BSP texture name (lowercased,
# `*` as `#`) its variants, each a set of pixels with the CRC the engine's
# texture cache keys on (CRC_Block of mip 0's 8-bit pixels, the bytes after
# the 40-byte miptex header as gl_model.c copies them: CRC-16 with the
# polynomial 0x1021, 0xffff first, not reflected), its size and the maps that
# have it. Prints how many names there are and how many have more than one
# variant (those get the `<name>~<crc>` file names), the ambiguous ones with
# their qualified names (-All: every name), and reports a CRC shared by two
# variants of one name (the qualifier couldn't tell them apart) and names with
# characters other than a-z 0-9 _ - + # (a file name needs a look). Sky
# textures (sky*) are listed too, with the same CRC, though the engine loads
# them as two layers of their own (vk_textures list shows upsky and lowsky;
# 5.5). Model skins and sprite frames aren't here: gl_model.c flood-fills a
# model's first skin before its CRC, so the export (5.4) takes theirs from the
# cache.
if (-not ('TexNamesCrc' -as [type])) {
Add-Type -TypeDefinition @'
public static class TexNamesCrc {
	static readonly ushort[] table = MakeTable();
	static ushort[] MakeTable() {
		var t = new ushort[256];
		for (int i = 0; i < 256; i++) {
			int c = i << 8;
			for (int k = 0; k < 8; k++) c = ((c & 0x8000) != 0) ? (c << 1) ^ 0x1021 : c << 1;
			t[i] = (ushort)c;
		}
		return t;
	}
	public static ushort Block(byte[] b, int start, int count) {
		ushort crc = 0xffff;
		for (int i = 0; i < count; i++) crc = (ushort)((crc << 8) ^ table[(crc >> 8) ^ b[start + i]]);
		return crc;
	}
}
'@
}

$names = @{}	# name -> crc -> @{ Size; Maps }
foreach ($pak in $Paks) {
	$b = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($b, 4)
	$dirlen = [BitConverter]::ToInt32($b, 8)
	for ($e = 0; $e -lt $dirlen / 64; $e++) {
		$o = $dirofs + $e * 64
		$file = [Text.Encoding]::ASCII.GetString($b, $o, 56).Split([char]0)[0]
		if ($file -notmatch '^maps/(.*)\.bsp$') { continue }
		$map = $Matches[1]
		$pos = [BitConverter]::ToInt32($b, $o + 56)
		if ([BitConverter]::ToInt32($b, $pos + 4 + 2 * 8 + 4) -eq 0) { continue }	# no textures lump
		$tex = $pos + [BitConverter]::ToInt32($b, $pos + 4 + 2 * 8)	# lump 2, the textures
		$n = [BitConverter]::ToInt32($b, $tex)
		for ($i = 0; $i -lt $n; $i++) {
			$mo = [BitConverter]::ToInt32($b, $tex + 4 + 4 * $i)
			if ($mo -lt 0) { continue }
			$m = $tex + $mo
			$name = [Text.Encoding]::ASCII.GetString($b, $m, 16).Split([char]0)[0].ToLowerInvariant().Replace('*', '#')
			$w = [BitConverter]::ToInt32($b, $m + 16)
			$h = [BitConverter]::ToInt32($b, $m + 20)
			$crc = '{0:x4}' -f [TexNamesCrc]::Block($b, $m + 40, $w * $h)
			if (-not $names[$name]) { $names[$name] = @{} }
			$v = $names[$name][$crc]
			if (-not $v) { $v = $names[$name][$crc] = @{ Size = "${w}x$h"; Maps = New-Object System.Collections.Generic.List[string]; Hashes = @{} } }
			if (-not $v.Maps.Contains($map)) { $v.Maps.Add($map) }
			# a second pixel set under the same name and CRC would be a collision
			$sha = [BitConverter]::ToString([Security.Cryptography.SHA1]::Create().ComputeHash($b, $m + 40, $w * $h))
			$v.Hashes["$sha/${w}x$h"] = 1
		}
	}
}

$ambiguous = @($names.Keys | Where-Object { $names[$_].Count -gt 1 } | Sort-Object)
"{0} texture names, {1} with more than one variant (named <name>~<crc>)" -f $names.Count, $ambiguous.Count
foreach ($name in ($names.Keys | Sort-Object)) {
	foreach ($crc in $names[$name].Keys) {
		if ($names[$name][$crc].Hashes.Count -gt 1) { "COLLISION: $name has different pixels with the CRC $crc" }
	}
	if ($name -notmatch '^[a-z0-9_+#-]+$') { "NAME: '$name' has characters a file name may not take" }
}
foreach ($name in ($names.Keys | Sort-Object)) {
	if (-not $All -and $names[$name].Count -eq 1) { continue }
	foreach ($crc in ($names[$name].Keys | Sort-Object)) {
		$v = $names[$name][$crc]
		$file = if ($names[$name].Count -gt 1) { "$name~$crc" } else { $name }
		"{0,-20} crc {1} {2,-8} {3}" -f $file, $crc, $v.Size, ($v.Maps -join ',')
	}
}
