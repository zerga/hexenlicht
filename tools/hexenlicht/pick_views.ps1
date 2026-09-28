# Picks calibration views in a hub's main areas (story 4.11; docs/hexenlicht/
# TESTING.md, "Calibrating a hub"): per map in the paks, -PerMap
# info_player_deathmatch spots (the mappers placed them in the main areas, on
# the floor), each as far as possible from the views already taken (the
# -Entrances bookmark file's views of that map, then the ones picked before;
# farthest-point sampling). A view is the player's origin on the floor below the
# spot and the spot's angle, or, when that faces a wall (the mean distance to
# solid or sky over a 60-degree fan at the eye, 50 units up, is under 200
# units), the most open of 16 yaws; spots less open than 160 units, with the
# eye not in open space (solid, water, sky), or with an entity whose whole
# classname matches -Avoid (a regex, default monster_.*; '' for none) less than
# -AvoidRadius (48) units across and 128 up or down from the player's origin
# are left out (4.11b: a monster standing on the spot puts the camera inside
# its model). The distances come from point traces through the BSP's nodes
# (hull 0, the world only: brush entities such as doors are open space).
# Writes bookmark lines (name <map>_dm<n>, rename them by place) to -Out, each
# after a comment with its openness and distance, and prints them. -Portals:
# Portal of Praevus maps (the bookmarks get "portals"). BSP version 29 only;
# needs PowerShell 7.
param([Parameter(Mandatory)][string[]]$Paks, [Parameter(Mandatory)][string[]]$Maps, [string]$Entrances = '',
      [int]$PerMap = 3, [Parameter(Mandatory)][string]$Out, [switch]$Portals, [string]$Avoid = 'monster_.*',
      [double]$AvoidRadius = 48)
$ErrorActionPreference = 'Stop'
if (-not ('BspTrace' -as [type])) {
Add-Type -TypeDefinition @'
using System;
public class BspTrace {
	float[] pn; float[] pd; int[] npl; int[] nc0; int[] nc1; int[] lc;
	// a map's planes (lump 1), nodes (5) and leafs (10), BSP 29, at pos in the pak
	public BspTrace(byte[] b, int pos) {
		if (BitConverter.ToInt32(b, pos) != 29) throw new Exception("not a BSP 29 map (BSP2 and others are not read)");
		int ofs(int l) { return BitConverter.ToInt32(b, pos + 4 + l * 8); }
		int len(int l) { return BitConverter.ToInt32(b, pos + 8 + l * 8); }
		int np = len(1) / 20; pn = new float[np * 3]; pd = new float[np];
		for (int i = 0; i < np; i++) { int o = pos + ofs(1) + i * 20; for (int k = 0; k < 3; k++) pn[i * 3 + k] = BitConverter.ToSingle(b, o + k * 4); pd[i] = BitConverter.ToSingle(b, o + 12); }
		int nn = len(5) / 24; npl = new int[nn]; nc0 = new int[nn]; nc1 = new int[nn];
		for (int i = 0; i < nn; i++) { int o = pos + ofs(5) + i * 24; npl[i] = BitConverter.ToInt32(b, o); nc0[i] = BitConverter.ToInt16(b, o + 4); nc1[i] = BitConverter.ToInt16(b, o + 6); }
		int nl = len(10) / 28; lc = new int[nl];
		for (int i = 0; i < nl; i++) lc[i] = BitConverter.ToInt32(b, pos + ofs(10) + i * 28);
	}
	// the contents of the leaf at a point (-1 empty, -2 solid, -6 sky)
	public int Contents(double x, double y, double z) {
		int n = 0;
		while (n >= 0) { int p = npl[n]; double d = pn[p * 3] * x + pn[p * 3 + 1] * y + pn[p * 3 + 2] * z - pd[p]; n = d >= 0 ? nc0[n] : nc1[n]; }
		return lc[-1 - n];
	}
	// the distance to the first solid or sky along a direction, in steps of 4, at most max
	public double Dist(double x, double y, double z, double dx, double dy, double dz, double max) {
		for (double t = 4; t <= max; t += 4) { int c = Contents(x + dx * t, y + dy * t, z + dz * t); if (c == -2 || c == -6) return t; }
		return max;
	}
	// the floor under a point: the first solid at most 256 units down, NaN = none
	public double Floor(double x, double y, double z) {
		for (double t = 0; t <= 256; t += 1) { if (Contents(x, y, z - t) == -2) return z - t + 1; }
		return double.NaN;
	}
}
'@
}
$inv = [Globalization.CultureInfo]::InvariantCulture

# the views taken already, per map
$taken = @{}
if ($Entrances) {
	foreach ($line in Get-Content $Entrances) {
		$t = $line.Trim()
		if (-not $t -or $t.StartsWith('#')) { continue }
		$w = $t -split '\s+'
		if (-not $taken[$w[1]]) { $taken[$w[1]] = [Collections.Generic.List[object]]::new() }
		$taken[$w[1]].Add(@([double]::Parse($w[2], $inv), [double]::Parse($w[3], $inv), [double]::Parse($w[4], $inv)))
	}
}

# the mean distance over a fan of +-30 degrees at pitch 0 and 10 down, capped at 1024
function Openness([BspTrace]$bt, [double]$x, [double]$y, [double]$z, [double]$yaw) {
	$s = 0; $n = 0
	foreach ($da in -30, -15, 0, 15, 30) {
		foreach ($pa in 0, 10) {
			$a = ($yaw + $da) * [Math]::PI / 180; $p = -$pa * [Math]::PI / 180
			$s += $bt.Dist($x, $y, $z, [Math]::Cos($a) * [Math]::Cos($p), [Math]::Sin($a) * [Math]::Cos($p), [Math]::Sin($p), 1024); $n++
		}
	}
	$s / $n
}

$lines = [Collections.Generic.List[string]]::new()
foreach ($pak in $Paks) {
	$bytes = [IO.File]::ReadAllBytes($pak)
	$dirofs = [BitConverter]::ToInt32($bytes, 4); $dirlen = [BitConverter]::ToInt32($bytes, 8)
	for ($e = 0; $e -lt $dirlen / 64; $e++) {
		$o = $dirofs + $e * 64
		$name = [Text.Encoding]::ASCII.GetString($bytes, $o, 56).Split([char]0)[0]
		if ($name -notmatch '^maps/(.*)\.bsp$') { continue }
		$map = $Matches[1]
		if ($Maps -notcontains $map) { continue }
		$pos = [BitConverter]::ToInt32($bytes, $o + 56)
		$bt = [BspTrace]::new($bytes, $pos)
		$entofs = [BitConverter]::ToInt32($bytes, $pos + 4); $entlen = [BitConverter]::ToInt32($bytes, $pos + 8)
		$text = [Text.Encoding]::ASCII.GetString($bytes, $pos + $entofs, $entlen)
		# the entities to keep away from (monsters: the camera would be inside one)
		$avoidAt = [Collections.Generic.List[object]]::new()
		foreach ($m in [regex]::Matches($text, '\{[^{}]*\}')) {
			if ($Avoid -and $m.Value -match '"classname"\s+"([^"]*)"' -and $Matches[1] -match "^(?:$Avoid)$" -and
			    $m.Value -match '"origin"\s+"([^"]*)"') {
				$avoidAt.Add(@($Matches[1] -split '\s+' | ForEach-Object { [double]::Parse($_, $inv) }))
			}
		}
		$cands = [Collections.Generic.List[object]]::new()
		foreach ($m in [regex]::Matches($text, '\{[^{}]*\}')) {
			$v = $m.Value
			if ($v -notmatch '"classname"\s+"info_player_deathmatch"' -or $v -notmatch '"origin"\s+"([^"]*)"') { continue }
			$xyz = $Matches[1] -split '\s+' | ForEach-Object { [double]::Parse($_, $inv) }
			$yaw = if ($v -match '"angle"\s+"([^"]*)"') { [double]::Parse($Matches[1], $inv) } else { 0 }
			$floor = $bt.Floor($xyz[0], $xyz[1], $xyz[2])
			if ([double]::IsNaN($floor) -or $bt.Contents($xyz[0], $xyz[1], $floor + 50) -ne -1) { continue }
			# a monster where the player would stand (across, and up or down from the player's origin)
			$near = $avoidAt | Where-Object { [Math]::Sqrt(($_[0] - $xyz[0]) * ($_[0] - $xyz[0]) + ($_[1] - $xyz[1]) * ($_[1] - $xyz[1])) -lt $AvoidRadius -and
							  [Math]::Abs($_[2] - $floor) -lt 128 }
			if ($near) { continue }
			$open = Openness $bt $xyz[0] $xyz[1] ($floor + 50) $yaw
			if ($open -lt 200) {
				for ($a = 0; $a -lt 360; $a += 22.5) {
					$q = Openness $bt $xyz[0] $xyz[1] ($floor + 50) $a
					if ($q -gt $open) { $open = $q; $yaw = $a }
				}
			}
			if ($open -ge 160) { $cands.Add([pscustomobject]@{ X = $xyz[0]; Y = $xyz[1]; Z = $floor; Yaw = $yaw; Open = $open }) }
		}
		$fixed = [Collections.Generic.List[object]]::new()
		if ($taken[$map]) { $fixed.AddRange($taken[$map]) }
		for ($k = 1; $k -le $PerMap -and $cands.Count; $k++) {
			$best = $null; $bd = -1.0
			foreach ($c in $cands) {
				$d = [double]::MaxValue
				foreach ($f in $fixed) {
					$dd = [Math]::Sqrt(($c.X - $f[0]) * ($c.X - $f[0]) + ($c.Y - $f[1]) * ($c.Y - $f[1]) + ($c.Z - $f[2]) * ($c.Z - $f[2]))
					$d = [Math]::Min($d, $dd)
				}
				if ($d -gt $bd) { $bd = $d; $best = $c }
			}
			$fixed.Add(@($best.X, $best.Y, $best.Z)); [void]$cands.Remove($best)
			$lines.Add(('# {0}_dm{1}: open {2}, {3} from the other views' -f $map, $k, [Math]::Round($best.Open).ToString($inv),
				$(if ($bd -eq [double]::MaxValue) { 'none' } else { [Math]::Round($bd).ToString($inv) })))
			$lines.Add(('{0}_dm{1} {0} {2} {3} {4} 0.0 {5}{6}' -f $map, $k, $best.X.ToString('F1', $inv), $best.Y.ToString('F1', $inv),
				$best.Z.ToString('F1', $inv), $best.Yaw.ToString('F1', $inv), $(if ($Portals) { ' portals' } else { '' })))
		}
	}
}
Set-Content -Path $Out -Value $lines
$lines
