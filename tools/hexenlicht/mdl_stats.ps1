param([string[]]$Paks)
# prints numtris, numverts, numframes (and pose count estimate) of every .mdl in the paks
$rows = @()
foreach ($pak in $Paks) {
    $bytes = [System.IO.File]::ReadAllBytes($pak)
    $dirofs = [BitConverter]::ToInt32($bytes, 4)
    $dirlen = [BitConverter]::ToInt32($bytes, 8)
    for ($o = $dirofs; $o -lt $dirofs + $dirlen; $o += 64) {
        $name = [System.Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
        if (-not $name.EndsWith('.mdl')) { continue }
        $pos = [BitConverter]::ToInt32($bytes, $o + 56)
        $ident = [System.Text.Encoding]::ASCII.GetString($bytes, $pos, 4)
        $h = $pos + 8 + 12 + 12 + 4 + 12
        $numskins = [BitConverter]::ToInt32($bytes, $h)
        $numverts = [BitConverter]::ToInt32($bytes, $h + 12)
        $numtris = [BitConverter]::ToInt32($bytes, $h + 16)
        $numframes = [BitConverter]::ToInt32($bytes, $h + 20)
        $flags = [BitConverter]::ToInt32($bytes, $h + 28)
        $rows += [pscustomobject]@{ pak = [System.IO.Path]::GetFileName($pak); name = $name; ident = $ident; tris = $numtris; verts = $numverts; frames = $numframes; skins = $numskins; flags = ('0x{0:x}' -f $flags) }
    }
}
$rows | Sort-Object tris -Descending | Select-Object -First 25 | Format-Table -AutoSize | Out-String -Width 200
"models: {0}, max tris {1}, sum tris {2}, avg tris {3:N0}" -f $rows.Count, ($rows | Measure-Object tris -Maximum).Maximum, ($rows | Measure-Object tris -Sum).Sum, ($rows | Measure-Object tris -Average).Average
"sum verts*frames*4 bytes (approx pose data before strip duplication): {0:N1} MB" -f ((($rows | ForEach-Object { $_.verts * $_.frames * 4 }) | Measure-Object -Sum).Sum / 1MB)
"max frames {0}" -f ($rows | Measure-Object frames -Maximum).Maximum
$rows | Where-Object { $_.ident -ne 'IDPO' } | Measure-Object | ForEach-Object { "RAPO models: {0}" -f $_.Count }
