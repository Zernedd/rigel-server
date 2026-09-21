<#
.SYNOPSIS
  Build the Spec Editor mod (dsound.dll).
.DESCRIPTION
  Compiles the proxy + editor (render half, UI, game half) together with Dear ImGui and MinHook into a
  single dsound.dll. Same one-cmd-session shape as the Rigel EOS redirect build: vcvars64, then ml64 for
  the ordinal thunks, then cl, chained with && only so the first failure stops everything, finishing
  with a BUILD_OK marker. Do NOT add `if errorlevel` steps to that chain - cmd folds the next command
  into the if's body and silently skips the compile, which once shipped a stale DLL that looked fine.
.EXAMPLE
  .\build.ps1
#>
$ErrorActionPreference = 'Stop'
$here   = $PSScriptRoot
$root   = Resolve-Path (Join-Path $here '..')
$mh     = (Resolve-Path (Join-Path $here '..\..\HalcyonA2\ThirdParty\MinHook')).Path
$imgui  = (Resolve-Path (Join-Path $root 'ThirdParty\imgui')).Path
$sdk    = (Resolve-Path (Join-Path $here '..\..\HalcyonA2\HalcyonA2\gamesdk\22284')).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No VS C++ toolset found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The generated SDK is enormous; /bigobj and a large /Zm are both required or cl runs out of heap.
$cl = "cl /nologo /c /O2 /MT /EHsc /GS- /std:c++20 /bigobj /Zm400 " +
      "/I`"$mh\include`" /I`"$imgui`" /I`"$imgui\backends`" /I`"$sdk`" " +
      "/DIMGUI_DISABLE_OBSOLETE_FUNCTIONS "

$cmds = @(
  "call `"$vcvars`"",
  "cd /d `"$out`"",
  "ml64 /nologo /c /Fo thunks.obj `"$here\thunks.asm`"",
  "$cl `"$here\dllmain.cpp`" `"$here\se_render.cpp`" `"$here\se_ui.cpp`" `"$here\se_game.cpp`" `"$here\se_sdk_glue.cpp`"",
  "$cl `"$sdk\SDK\Basic.cpp`" `"$sdk\SDK\CoreUObject_functions.cpp`"",
  "$cl `"$imgui\imgui.cpp`" `"$imgui\imgui_draw.cpp`" `"$imgui\imgui_tables.cpp`" `"$imgui\imgui_widgets.cpp`" `"$imgui\backends\imgui_impl_dx12.cpp`" `"$imgui\backends\imgui_impl_win32.cpp`"",
  "cl /nologo /c /O2 /MT `"$mh\src\buffer.c`" `"$mh\src\hook.c`" `"$mh\src\trampoline.c`" `"$mh\src\hde\hde64.c`"",
  "cl /nologo /LD /Fe:dsound.dll *.obj /link /DEF:`"$here\exports.def`" kernel32.lib user32.lib d3d12.lib dxgi.lib",
  "echo BUILD_OK"
) -join ' && '

$log = & cmd.exe /c $cmds 2>&1
$log | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -or -not ($log -match 'BUILD_OK')) { throw "build failed (exit $LASTEXITCODE)" }

Copy-Item (Join-Path $out 'dsound.dll') (Join-Path $here 'dsound.dll') -Force
$sz = (Get-Item (Join-Path $here 'dsound.dll')).Length
Write-Host "[spec-editor] -> $(Join-Path $here 'dsound.dll')  ($sz bytes)"
