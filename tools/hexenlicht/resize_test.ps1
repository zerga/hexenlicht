# Resizes the game window from outside while a test script runs: starts
# hexenlicht.exe (1280x720 window) with data1\hl_test.cfg and, each time the
# script echoes "==== STEP<n>" into the log (-condebug), does step n:
# 1 resize to 1000x700, 2 maximize, 3 restore, 4 too small (100x100),
# 5 resize to 1000x700. The script should wait a few hundred frames after each
# echo. The game data folder is -Data, else $env:HEXENLICHT_DATA, else
# Hexenlicht-data next to the repository.
param([string]$Exe = '', [string]$Data = '')
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Exe) { $Exe = Join-Path $repo 'build\windows-debug\bin\hexenlicht.exe' }
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr FindWindow(string cls, string name);
}
"@
[W]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null	# physical pixels, like the game

$log = Join-Path $Data 'debug_h2.log'
function WaitMark($m) {
	for ($t = 0; $t -lt 600; $t++) {
		if ((Test-Path $log) -and (Select-String -Path $log -Pattern $m -SimpleMatch -Quiet)) { return }
		Start-Sleep -Milliseconds 200
	}
	throw "timeout waiting for $m"
}
function Client($h) { $c = New-Object W+RECT; [W]::GetClientRect($h, [ref]$c) | Out-Null; "$($c.R)x$($c.B)" }
function SetClient($h, $w, $hh) {
	$wr = New-Object W+RECT; $cr = New-Object W+RECT
	[W]::GetWindowRect($h, [ref]$wr) | Out-Null; [W]::GetClientRect($h, [ref]$cr) | Out-Null
	$dx = ($wr.R - $wr.L) - $cr.R; $dy = ($wr.B - $wr.T) - $cr.B
	[W]::SetWindowPos($h, [IntPtr]::Zero, $wr.L, $wr.T, $w + $dx, $hh + $dy, 0x14) | Out-Null	# NOZORDER|NOACTIVATE
}

Remove-Item $log -ErrorAction SilentlyContinue	# the last run's marks would match at once
$p = Start-Process $Exe -ArgumentList "-window -width 1280 -height 720 -condebug +exec hl_test.cfg" -WorkingDirectory $Data -PassThru
try {
	WaitMark '==== STEP1'
	$h = [W]::FindWindow('HexenII', 'Hexenlicht')
	"start client $(Client $h)"
	SetClient $h 1000 700;	"step1 resize -> $(Client $h)"
	WaitMark '==== STEP2'
	[W]::ShowWindow($h, 3) | Out-Null;	"step2 maximize -> $(Client $h)"
	WaitMark '==== STEP3'
	[W]::ShowWindow($h, 9) | Out-Null;	"step3 restore -> $(Client $h)"
	WaitMark '==== STEP4'
	SetClient $h 100 100;	"step4 too small -> $(Client $h)"
	WaitMark '==== STEP5'
	SetClient $h 1000 700;	"step5 resize -> $(Client $h)"
	if (-not $p.WaitForExit(60000)) { "TIMEOUT" }
	else { "exit $($p.ExitCode)" }
}
finally {
	if (-not $p.HasExited) { $p.Kill() }
}
