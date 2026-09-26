# Runs hexenlicht.exe or glh2.exe from the game data folder with a test script
# (exec'd at startup, e.g. data1\hl_test.cfg) and prints how it ended.
# The build is this repository's build\<preset>\bin, or -Bin (e.g. an older
# build in another worktree). The game data folder is -Data, else
# $env:HEXENLICHT_DATA, else Hexenlicht-data next to the repository.
# See docs/hexenlicht/TESTING.md.
param([string]$Exe = 'hexenlicht', [string]$Cfg = 'hl_test.cfg', [switch]$Portals, [switch]$Release,
      [int]$Timeout = 150, [int]$Width = 960, [int]$Height = 540, [string]$Data = '', [string]$Bin = '')
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Data) { $Data = if ($env:HEXENLICHT_DATA) { $env:HEXENLICHT_DATA } else { Join-Path (Split-Path $repo) 'Hexenlicht-data' } }
$preset = if ($Release) { 'windows-release' } else { 'windows-debug' }
if (-not $Bin) { $Bin = Join-Path $repo "build\$preset\bin" }
$name = if ($Exe -eq 'glh2') { 'glh2.exe' } else { 'hexenlicht.exe' }
$argList = "-window -width $Width -height $Height -condebug"
if ($Portals) { $argList += " -portals" }
$argList += " +exec $Cfg"
$p = Start-Process (Join-Path $Bin $name) -ArgumentList $argList -WorkingDirectory $Data -PassThru
if (-not $p.WaitForExit($Timeout * 1000)) { "TIMEOUT"; $p.Kill() } else { "exit $($p.ExitCode)" }
