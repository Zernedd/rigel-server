<#
.SYNOPSIS
  Starts a headless MOCK CLIENT: a second copy of the shipping exe with HalcyonA2.dll injected in
  -HalcyonClient mode, which joins a HalcyonA2 server, leaves spectator via
  ASpectatorCameraManagerPawn::Server_ExitSpectator, and logs pawn / ball / VOIP state.

.EXAMPLE
  .\Start-Client.ps1                       # connect to 127.0.0.1:7777
  .\Start-Client.ps1 -Connect 127.0.0.1:7778
#>
param(
    [string] $Connect = '127.0.0.1:7777',
    [switch] $Wasd,
    [string] $Config  = 'server.config.psd1',
    [int]    $InitWaitSeconds = 25,
    # [BALLTEST] -Name gives this client its own log (%TEMP%\HalcyonA2-client<Name>.log) so two can
    # run side by side; -Drive makes it the one that acts on a ball (enter arena, hit, spawn).
    [string] $Name = '',
    [switch] $Drive,
    [string] $Arena = 'TKB_Prime',
    # Test script (one command per line: enter <SlotID> | goto x y z [secs] | wait secs | log text).
    [string] $Script = '',
    # Don't spawn the ball-spam test balls.
    [switch] $NoBalls,
    # Log-only hooks on the pawn's team join / colour / clear (value + caller).
    [switch] $TeamHooks,
    # World netvar path to read from this client's own copy every 5s (e.g. config/player/brakeStrength).
    [string] $NvRead = '',
    # Log this client's quest component / ClientProgression state every 5s ([QSTATE]).
    [switch] $QuestState,
    [switch] $VoipState
)
$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "[client] $m" -ForegroundColor Cyan }

$cfg = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot $Config)
$exe = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $cfg.GameExe))
$dll = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\build\$($cfg.GameBuild)\x64\$($cfg.Configuration)\HalcyonA2.dll"))
if (-not (Test-Path $exe)) { throw "Game exe not found: $exe" }
if (-not (Test-Path $dll)) { throw "Payload not found: $dll" }

$logDir = Join-Path $PSScriptRoot $cfg.LogDir
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$out    = Join-Path $logDir "client-$stamp.out.log"
$err    = Join-Path $logDir "client-$stamp.err.log"

# A client keeps -nullrhi/-nohmd/-nosound so it runs headless on this box, and carries the two
# flags the payload parses in client mode.
$args = @('-nullrhi','-nohmd','-nosound','-unattended','-nosplash','-log',
          '-HalcyonClient', "-HalcyonConnect=$Connect")
if ($Name)  { $args += "-HalcyonName=$Name" }
if ($Drive) { $args += '-HalcyonDrive'; $args += "-HalcyonArena=$Arena" }
if ($Wasd -or $Drive) { $args += '-HalcyonWASD' }  # test clients that need to move
if ($Script)  { $args += "-HalcyonScript=$([System.IO.Path]::GetFullPath($Script))" }
if ($NoBalls) { $args += '-HalcyonNoBalls' }
if ($TeamHooks) { $args += '-HalcyonTeamHooks' }
if ($NvRead)    { $args += "-HalcyonNvRead=$NvRead" }
if ($QuestState) { $args += '-HalcyonQuestState' }
if ($VoipState)  { $args += '-HalcyonVoipState' }

Info "launching client$(if ($Name) { " $Name" })$(if ($Drive) { " [DRIVER]" }) -> $Connect"
$p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
Info "  pid $($p.Id)  log $out"
Info "  waiting ${InitWaitSeconds}s for engine init"
Start-Sleep -Seconds $InitWaitSeconds
if ($p.HasExited) { throw "client pid $($p.Id) exited (code $($p.ExitCode)) before injection" }
& (Join-Path $PSScriptRoot 'Inject.ps1') -ProcessId $p.Id -DllPath $dll
Info "injected. payload log: $env:TEMP\HalcyonA2-client$Name.log"
Info "stop with: Stop-Process -Id $($p.Id)"
