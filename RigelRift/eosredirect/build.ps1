<#
.SYNOPSIS
  Build the Rigel EOS redirect, shipped as the game binary dsound.dll.
.DESCRIPTION
  Compiles the dsound proxy + EOS curl-redirect and MinHook (from the HalcyonA2 tree, same as that
  project) into dsound.dll, assembling the ordinal thunks with ml64. Uses the VS x64 developer
  environment (vcvars64) so INCLUDE/LIB resolve without hand-built paths. Output: .\dsound.dll. Copy it
  into a build's A2\Binaries\Win64 (make_rift.py --dst <build> --eos does this).
.EXAMPLE
  .\build.ps1
#>
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mh   = (Resolve-Path (Join-Path $here '..\..\HalcyonA2\ThirdParty\MinHook')).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No VS C++ toolset found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# One cmd session: set up the x64 toolchain, then assemble + compile + link inside it. Chain with && only
# (which already stops on the first failure) and finish with a BUILD_OK marker. Do NOT insert
# `if errorlevel` steps here: joined with &&, cmd folds the next command into the if's body and silently
# skips the compile/link -- which once produced a stale DLL that looked like a success.
$cmds = @(
  "call `"$vcvars`"",
  "cd /d `"$out`"",
  "ml64 /nologo /c /Fo thunks.obj `"$here\thunks.asm`"",
  "cl /nologo /c /O2 /MT /EHsc /GS- /I`"$mh\include`" `"$here\dllmain.cpp`" `"$here\glyphfix.cpp`" `"$mh\src\buffer.c`" `"$mh\src\hook.c`" `"$mh\src\trampoline.c`" `"$mh\src\hde\hde64.c`"",
  "cl /nologo /LD /Fe:dsound.dll dllmain.obj glyphfix.obj buffer.obj hook.obj trampoline.obj hde64.obj thunks.obj /link /DEF:`"$here\exports.def`" kernel32.lib user32.lib psapi.lib",
  "echo BUILD_OK"
) -join ' && '

# With $ErrorActionPreference 'Stop', `2>&1` turns vcvars64's harmless "'vswhere.exe' is not recognized"
# stderr line into a terminating error before anything compiles. Let stderr be data; BUILD_OK decides.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try   { $log = & cmd.exe /c $cmds 2>&1 }
finally { $ErrorActionPreference = $prevEap }
$log | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -or -not ($log -match 'BUILD_OK')) { throw "build failed (exit $LASTEXITCODE)" }

Copy-Item (Join-Path $out 'dsound.dll') (Join-Path $here 'dsound.dll') -Force
$sz = (Get-Item (Join-Path $here 'dsound.dll')).Length
Write-Host "[eosredirect] -> $(Join-Path $here 'dsound.dll')  ($sz bytes)"

# Also refresh the publish build's copy if it exists, so a rebuild never leaves RiftPublish stale
# (that trap cost a full deploy+launch cycle once: the rebuilt DLL sat here while RiftPublish shipped
# the old one).
$publish = Join-Path $here '..\..\RiftPublish\A2\Binaries\Win64\dsound.dll'
if (Test-Path (Split-Path $publish)) {
    Copy-Item (Join-Path $here 'dsound.dll') $publish -Force
    Write-Host "[eosredirect] -> $((Resolve-Path $publish).Path)  (RiftPublish synced)"
}
