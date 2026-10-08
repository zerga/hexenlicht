param([string]$Data = '', [string[]]$Paks, [string]$Model = '', [string]$Csv = '', [switch]$List,
      [string]$Viewer = '', [string[]]$ViewerModels = @('pak1.pak:models/imp.mdl', 'pak3.pak:models/imp.mdl', 'pak0.pak:models/paladin.mdl', 'pak1.pak:models/golem_b.mdl', 'pak1.pak:models/medusa2.mdl', 'pak1.pak:models/boss/smaleido.mdl'),
      [string]$Sequence = '', [double]$StepBelow = 0, [double]$StepAbove = 0, [switch]$AreaWeighted, [switch]$Weld,
      [double]$Rho = 1.0, [double]$Relax = 1.6, [double]$Eps = 1e-3, [int]$MaxIters = 2000, [double]$LoopRatio = 1.5)
# Story 8.1's prototype (mdl_smooth.cs): smooths every alias model's
# animation within the 8-bit vertices' rounding cells and reports the shake
# before and after, per model and in total. -Model a regex on the model's
# name; -List prints each model's sequences (frames, loop or not); -Csv the
# per-model numbers; -Weld shares one normal between a UV seam's duplicates
# (DECISIONS G14); -Viewer <file.html> writes mdl_smooth_viewer.html with
# the -ViewerModels (pak:name) in it: Raven's vertex data, so outside the
# repository, never published. The paks: -Paks, else data1's pak0/pak1 and
# portals' pak3 in -Data ($env:HEXENLICHT_DATA, else Hexenlicht-data next to
# the repository). See docs/hexenlicht/TESTING.md "Model smoothing (8.1, 8.2)".
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$abs = { param($p) $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($p) }
if (-not $Paks) { $Paks = @('data1\pak0.pak', 'data1\pak1.pak', 'portals\pak3.pak') | ForEach-Object { Join-Path $Data $_ } | Where-Object { Test-Path $_ } }
$Paks = @($Paks | ForEach-Object { & $abs $_ })
if (-not $Paks) { throw "no paks: none in $Data (-Data, -Paks)" }
if ($Viewer) {
	# the viewer holds Raven's vertex data: never inside a git working tree (this
	# repository, its worktrees, any other)
	if ($AreaWeighted) { throw "-Viewer: the viewer rebuilds normals unit-weighted; leave out -AreaWeighted" }
	$Viewer = & $abs $Viewer
	$dir = Split-Path $Viewer
	if (-not (Test-Path -PathType Container $dir)) { throw "-Viewer: no folder $dir" }
	# outside only when git says so: no git, or another error, refuses
	if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "-Viewer: needs git on PATH to check that $dir is outside any working tree" }
	$out = git -C $dir rev-parse --is-inside-work-tree 2>&1
	$outside = ($out -join "`n") -eq 'false' -or ($out -join "`n") -match 'not a git repository'
	$global:LASTEXITCODE = 0
	if (-not $outside -or $Viewer.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "-Viewer: $dir is in a git working tree; the viewer holds Raven's vertex data, write it elsewhere" }
}
Add-Type -Path (Join-Path $PSScriptRoot 'mdl_smooth.cs')
[MdlSmooth]::LoadNormals((Join-Path $repo 'common\anorms.h'))
[MdlSmooth]::Rho = $Rho; [MdlSmooth]::Relax = $Relax; [MdlSmooth]::Eps = $Eps
[MdlSmooth]::MaxIters = $MaxIters; [MdlSmooth]::LoopRatio = $LoopRatio
[MdlSmooth]::SeqFilter = $Sequence; [MdlSmooth]::StepBelow = $StepBelow; [MdlSmooth]::StepAbove = $StepAbove; [MdlSmooth]::AreaWeighted = [bool]$AreaWeighted; [MdlSmooth]::Weld = [bool]$Weld

$models = foreach ($pak in $Paks) { [MdlSmooth]::ReadPak($pak, $Model) }
$stats = foreach ($m in $models) { [MdlSmooth]::Smooth($m) }

if ($List) {
	foreach ($m in $models) {
		"{0}:{1}  {2} poses, {3} verts" -f $m.Pak, $m.Name, $m.NumPoses, $m.NumVerts
		foreach ($s in $m.Seqs) {
			$kind = if ($s.Group) { 'group' } elseif ($s.Count -lt 3) { 'short' } elseif ($s.Loop) { 'loop' } else { 'once' }
			"    {0,-16} {1,4}..{2,-4} {3,-5} wrap {4,6:F2} step {5,6:F2}  {6} .. {7}" -f $s.Name, $s.Start, ($s.Start + $s.Count - 1), $kind, $s.Wrap, $s.Step, $m.PoseNames[$s.Start], $m.PoseNames[$s.Start + $s.Count - 1]
		}
	}
}

$rows = $stats | Where-Object { $_.AccelBefore -gt 0 } | ForEach-Object { [pscustomobject]@{
	pak = $_.Pak; model = $_.Name; verts = $_.Verts; poses = $_.Poses; seqs = $_.Seqs; loops = $_.Loops
	accel = '{0:F2} -> {1:F2}' -f $_.AccelBefore, $_.AccelAfter
	edge_med = '{0:F2} -> {1:F2}' -f $_.EdgeMedBefore, $_.EdgeMedAfter
	edge_p90 = '{0:F2} -> {1:F2}' -f $_.EdgeP90Before, $_.EdgeP90After
	normal_jerk = '{0:F1} / {1:F1} / {2:F1}' -f $_.NrmTable, $_.NrmRebuilt, $_.NrmSmoothed
	over5 = '{0:F1} -> {1:F1}' -f $_.NrmTableOver5, $_.NrmSmoothedOver5
	ms = [math]::Round($_.Ms, 1); iters = '{0:F0}/{1}' -f $_.ItersMean, $_.ItersMax
} }
$rows | Sort-Object poses -Descending | Format-Table -AutoSize | Out-String -Width 250

$anim = @($stats | Where-Object { $_.AccelBefore -gt 0 })
function Median($v) { $s = @($v | Sort-Object); if (-not $s.Count) { 0 } elseif ($s.Count % 2) { $s[($s.Count - 1) / 2] } else { ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2 } }
"models {0} ({1} animated), sequences {2} ({3} loops, {4} frame groups, {5} of 2 poses left as they are)" -f @($stats).Count, $anim.Count,
	($stats | Measure-Object Seqs -Sum).Sum, ($stats | Measure-Object Loops -Sum).Sum, ($stats | Measure-Object Groups -Sum).Sum, ($stats | Measure-Object Unsmoothed -Sum).Sum
"second difference RMS (grid steps), median model: {0:F2} -> {1:F2}" -f (Median $anim.AccelBefore), (Median $anim.AccelAfter)
"edge variation / rounding alone, median model's median: {0:F2} -> {1:F2}; its 90th percentile: {2:F2} -> {3:F2}" -f (Median $anim.EdgeMedBefore), (Median $anim.EdgeMedAfter), (Median $anim.EdgeP90Before), (Median $anim.EdgeP90After)
"normal jerk (degrees), median model: table {0:F2}, rebuilt from the bytes {1:F2}, rebuilt smoothed {2:F2}; above 5 degrees {3:F1} % -> {4:F1} %" -f (Median $anim.NrmTable), (Median $anim.NrmRebuilt), (Median $anim.NrmSmoothed), (Median $anim.NrmTableOver5), (Median $anim.NrmSmoothedOver5)
"table normal vs rebuilt from the bytes (degrees), median model: {0:F1}" -f (Median $anim.TableVsRebuilt)
"round trip: {0} of {1} samples differ after rounding; largest shift {2:F3} grid steps" -f ($stats | Measure-Object RoundTripMismatch -Sum).Sum, ($stats | Measure-Object Samples -Sum).Sum, ($stats | Measure-Object MaxShift -Maximum).Maximum
"solver: {0:F0} ms in all, the slowest model {1:F0} ms ({2}); iterations per series: mean {3:F0}, max {4}; not converged {5}" -f ($stats | Measure-Object Ms -Sum).Sum,
	($stats | Measure-Object Ms -Maximum).Maximum, ($stats | Sort-Object Ms -Descending | Select-Object -First 1).Name, ($anim | Measure-Object ItersMean -Average).Average, ($stats | Measure-Object ItersMax -Maximum).Maximum, ($stats | Measure-Object NotConverged -Sum).Sum

if ($Csv) { $stats | Export-Csv -NoTypeInformation -Path $Csv; "wrote $Csv" }

if ($Viewer) {
	$json = foreach ($vm in $ViewerModels) {
		$pak, $name = $vm.Split(':', 2)
		$m = $models | Where-Object { $_.Pak -eq $pak -and $_.Name -eq $name } | Select-Object -First 1
		if (-not $m) { Write-Warning "$vm not found (is it within -Model?)"; continue }
		if (-not ($m.Seqs | Where-Object { $_.Count -gt 1 })) { Write-Warning "$vm has no animation"; continue }
		[MdlSmooth]::ViewerJson($m)
	}
	if (-not $json) { throw "-Viewer: none of the -ViewerModels found" }
	$template = Get-Content -Raw (Join-Path $PSScriptRoot 'mdl_smooth_viewer.html')
	$data = "const ANORMS = $([MdlSmooth]::NormalsJson());`nconst MODELS = [$($json -join ",`n")];"
	[IO.File]::WriteAllText($Viewer, $template.Replace('/*@DATA@*/', $data))
	"wrote $Viewer ($([math]::Round((Get-Item $Viewer).Length / 1MB, 1)) MB)"
}
