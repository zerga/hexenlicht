# Checks that the C struct of the global UBO (QVKUniformBuffer_t,
# engine/hexenlicht/shaders/global_ubo.h) has every member at the offset the
# shaders' std140 block has: reads glslang's reflection of debug_view.comp,
# generates a C program with offsetof checks, compiles it with MSVC (found
# with vswhere) and runs it. Run after changing GLOBAL_UBO_VAR_LIST; needs
# VULKAN_SDK.
param([string]$Sdk = $env:VULKAN_SDK)
if (-not $Sdk) { throw 'Set VULKAN_SDK or pass -Sdk' }
$repo = Split-Path (Split-Path $PSScriptRoot)
$shaders = Join-Path $repo 'engine\hexenlicht\shaders'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
	-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$tmp = Join-Path $env:TEMP 'hl_ubo_check'
New-Item -ItemType Directory -Force $tmp | Out-Null
$refl = & "$Sdk\Bin\glslangValidator.exe" -V --target-env vulkan1.3 -DVKPT_SHADER "-I$shaders" -q --reflect-all-block-variables -o (Join-Path $tmp 'x.spv') (Join-Path $shaders 'debug_view.comp')
$seen = @{}
$checks = foreach ($line in $refl) {
    if ($line -notmatch '^global_ubo\.([^:]+): offset (\d+)') { continue }
    $name = $Matches[1]; $off = $Matches[2]
    if ($name.Contains('.')) { $name = $name.Split('.')[0]; if (-not $name.EndsWith('[0]')) { continue } }
    if ($name.Contains('[')) { if (-not $name.EndsWith('[0]')) { continue }; $name = $name.Replace('[0]', '') }
    if ($seen.ContainsKey($name)) { continue }; $seen[$name] = 1
    "n++; if (offsetof(QVKUniformBuffer_t, $name) != $off) { printf(""$name`: C %zu, GLSL $off\n"", offsetof(QVKUniformBuffer_t, $name)); bad++; }"
}
@('#include <stddef.h>', '#include <stdio.h>', '#include "global_ubo.h"', 'int main(void) { int bad = 0, n = 0;') + $checks +
    @('printf("%d members checked, %d differ, C size %zu\n", n, bad, sizeof(QVKUniformBuffer_t)); return bad; }') |
    Set-Content (Join-Path $tmp 'ubo_check.c')
$cmd = "call ""$vcvars"" x64 >nul && cd /d ""$tmp"" && cl -nologo -W3 ""-I$shaders"" ubo_check.c -Fe:ubo_check.exe >cl.log 2>&1 && "".\ubo_check.exe"" || type cl.log"
cmd /c $cmd
exit $LASTEXITCODE
