<#
.SYNOPSIS
  Starts a HalcyonA2 server: launch the game headless, inject HalcyonA2.dll, report status.

.DESCRIPTION
  One command for the whole sequence:
    1. Read tools\server.config.psd1 (game path, args, dashboard settings, log dir).
    2. Optionally build first (-Build).
    3. Launch A2-Win64-Shipping.exe with the server arguments, stdout/stderr to
       logs\server-<stamp>.out.log / .err.log.
    4. Wait for the engine, inject build\x64\<Configuration>\HalcyonA2.dll.
    5. Wait until each instance binds its UE game port (7777+) and report it.

  Several instances can run at once: UE takes 7777, 7778, 7779... and the DLL registers
  whichever port it actually bound, so each server publishes its own ip:port.

.EXAMPLE
  .\Start-Server.ps1
  .\Start-Server.ps1 -Build                    # rebuild the DLL first
  .\Start-Server.ps1 -Instances 3              # three servers on 7777/7778/7779
  .\Start-Server.ps1 -Config server.vps.psd1
  .\Start-Server.ps1 -Status                   # what is running right now
  .\Start-Server.ps1 -Stop                     # stop every running server
#>
[CmdletBinding(DefaultParameterSetName = 'Start')]
param(
    [Parameter(ParameterSetName = 'Start')] [string] $Config = 'server.config.psd1',
    [Parameter(ParameterSetName = 'Start')] [int]    $Instances = 1,
    [Parameter(ParameterSetName = 'Start')] [switch] $Build,
    [Parameter(ParameterSetName = 'Start')] [switch] $NoWait,
    [Parameter(ParameterSetName = 'Status')] [switch] $Status,
    [Parameter(ParameterSetName = 'Stop')]   [switch] $Stop
)

$ErrorActionPreference = 'Stop'
function Info($m)  { Write-Host "[server] $m" -ForegroundColor Cyan }
function Good($m)  { Write-Host "[server] $m" -ForegroundColor Green }
function Warn2($m) { Write-Host "[server] $m" -ForegroundColor Yellow }

$procName = 'A2-Win64-Shipping'

# --- status / stop ---------------------------------------------------------------------
if ($Status) {
    $running = @(Get-Process -Name $procName -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { Info "no server processes running"; return }
    Info "$($running.Count) server process(es):"
    foreach ($p in $running) {
        $ports = (Get-NetUDPEndpoint -OwningProcess $p.Id -ErrorAction SilentlyContinue |
                  Where-Object { $_.LocalPort -ge 7777 -and $_.LocalPort -le 7877 } |
                  Select-Object -Expand LocalPort -Unique | Sort-Object) -join ','
        $up = [int]((Get-Date) - $p.StartTime).TotalMinutes
        "        pid $($p.Id)  up ${up}m  RAM $([int]($p.WorkingSet64/1MB))MB  udp: $(if ($ports) { $ports } else { '(none bound)' })"
    }
    return
}

if ($Stop) {
    $running = @(Get-Process -Name $procName -ErrorAction SilentlyContinue)
    if ($running.Count -eq 0) { Info "nothing to stop"; return }
    foreach ($p in $running) { Info "stopping pid $($p.Id)"; Stop-Process -Id $p.Id -Force }
    Good "stopped $($running.Count) server process(es)"
    return
}

# --- config ----------------------------------------------------------------------------
$configPath = if ([System.IO.Path]::IsPathRooted($Config)) { $Config } else { Join-Path $PSScriptRoot $Config }
if (-not (Test-Path -LiteralPath $configPath)) { throw "Config not found: $configPath" }
$cfg = Import-PowerShellDataFile -LiteralPath $configPath
Info "config: $configPath"

function Resolve-FromTools([string] $p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return $null }
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $p))
}

$exe = Resolve-FromTools $cfg.GameExe
if (-not (Test-Path -LiteralPath $exe)) { throw "Game exe not found: $exe" }

$gameBuild = if ($cfg.GameBuild) { $cfg.GameBuild } else { '22284' }

# --- preflight: Oculus platform DLLs ----------------------------------------------------
# A2-Win64-Shipping.exe DELAY-loads LibOVRPlatformImpl64_1.dll, which in turn HARD-imports
# LibOVRP2P64_1.dll. Either one missing = the same failure: the server boots, registers
# itself with the backend, then dies ~30s later with 0xC06D007E (STATUS_DELAY_LOAD_FAILED),
# leaving a ghost station advertised as online that nobody can join.
#
# On a dev PC this can appear to work with only Impl present, because the Meta Horizon app
# puts C:\Program Files\Meta Horizon\Support\oculus-runtime on PATH and Windows resolves
# the P2P dependency from there. A VPS has no Meta app and no such PATH entry, so BOTH
# files must sit next to the exe.
$exeDir = Split-Path -Parent $exe
$missingOvr = @('LibOVRPlatformImpl64_1.dll', 'LibOVRP2P64_1.dll') |
              Where-Object { -not (Test-Path -LiteralPath (Join-Path $exeDir $_)) }
if ($missingOvr.Count -gt 0) {
    Warn2 "missing Oculus platform DLL(s) in $exeDir :"
    foreach ($m in $missingOvr) { Warn2 "    $m" }
    Warn2 "Without these the server starts, registers, then dies ~30s later with 0xC06D007E"
    Warn2 "and leaves a ghost station listed as online."
    Warn2 "Copy them from a PC with the Meta Horizon app:"
    Warn2 "    C:\Program Files\Meta Horizon\Support\oculus-runtime\"
    $ans = Read-Host "Start anyway? (y/N)"
    if ($ans -notmatch '^[Yy]') { throw "Aborted: missing $($missingOvr -join ', ')" }
}

# --- preflight: root certificates -------------------------------------------------------
# The build ships no cacert.pem, so UE's libcurl trusts the WINDOWS root store. Without the
# Starfield/Amazon root, the game's https call to a2-sandboxprojects.s3.amazonaws.com fails
# with "unable to get local issuer certificate", and the HTTP completion handler then
# dereferences the null response (+0x46C56C1) and kills the server ~40s after boot.
# install.ps1 imports windows\certs\*.cer; this only warns if that never happened.
if (-not (Get-ChildItem Cert:\LocalMachine\Root -EA SilentlyContinue |
          Where-Object Thumbprint -eq '925A8F8D2C6D04E0665F596AFF22D863E8256F3F')) {
    Warn2 "Starfield Services Root CA G2 is NOT in the machine root store."
    Warn2 "The server will crash ~40s in on 'libcurl error 60 / unable to get local issuer certificate'."
    Warn2 "Fix (elevated):"
    Warn2 "    Import-Certificate -FilePath '$PSScriptRoot\certs\StarfieldServicesRootG2.cer' ``"
    Warn2 "        -CertStoreLocation Cert:\LocalMachine\Root"
}

if ($Build) {
    Info "building $($cfg.Configuration) for game build $gameBuild..."
    & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $cfg.Configuration -GameBuild $gameBuild
}
$dll = if ($cfg.DllPath) { Resolve-FromTools $cfg.DllPath }
       else { Resolve-FromTools "..\build\$gameBuild\x64\$($cfg.Configuration)\HalcyonA2.dll" }
if (-not (Test-Path -LiteralPath $dll)) {
    throw "Payload not found: $dll`nBuild it first:  .\Build.ps1 -Configuration $($cfg.Configuration) -GameBuild $gameBuild   (or pass -Build)"
}
Info "game build $gameBuild; payload: $dll ($([math]::Round((Get-Item $dll).Length/1KB)) KB, built $((Get-Item $dll).LastWriteTime))"

$logDir = Resolve-FromTools $cfg.LogDir
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

# --- build the command line ------------------------------------------------------------
# The DLL reads -Dashboard*/-RegisterIp/-LoadGamemode straight off the process command
# line, so anything set in the config has to be passed here, not via the environment
# (env vars do not reach the game process under the allocator).
$gameArgs = @($cfg.Args)
if ($cfg.DashboardApiUrl) { $gameArgs += "-DashboardApiUrl=$($cfg.DashboardApiUrl)" }
if ($cfg.DashboardApiKey) { $gameArgs += "-DashboardApiKey=$($cfg.DashboardApiKey)" }
if ($cfg.RegisterIp)      { $gameArgs += "-RegisterIp=$($cfg.RegisterIp)" }
if ($cfg.LoadGamemode)    { $gameArgs += "-LoadGamemode=$($cfg.LoadGamemode)" }
if ($cfg.GamemodeSlot)    { $gameArgs += "-GamemodeSlot=$($cfg.GamemodeSlot)" }
if ($cfg.ExtraArgs)       { $gameArgs += @($cfg.ExtraArgs) }

$dllLog = Join-Path $env:TEMP 'HalcyonA2.log'

# --- launch + inject --------------------------------------------------------------------
$started = @()
for ($i = 0; $i -lt $Instances; $i++) {
    $stamp = Get-Date -Format yyyyMMdd-HHmmss
    $tag = if ($Instances -gt 1) { "$stamp-$i" } else { $stamp }

    # Each instance gets its own deployment id, otherwise they collide in the backend.
    $instArgs = @($gameArgs)
    if ($cfg.DeploymentId) {
        $depId = if ($Instances -gt 1) { "$($cfg.DeploymentId)-$i" } else { $cfg.DeploymentId }
        $instArgs += "-DashboardDeploymentId=$depId"
    }

    $outLog = Join-Path $logDir "server-$tag.out.log"
    $errLog = Join-Path $logDir "server-$tag.err.log"

    Info "launching instance $($i + 1)/$Instances"
    # Redirect: without it the child inherits our console handles and anything waiting on
    # those pipes blocks until the GAME exits.
    $proc = Start-Process -FilePath $exe -ArgumentList $instArgs -WorkingDirectory (Split-Path -Parent $exe) `
                          -PassThru -RedirectStandardOutput $outLog -RedirectStandardError $errLog
    Info "  pid $($proc.Id)  log $outLog"

    Info "  waiting $($cfg.InitWaitSeconds)s for engine init"
    for ($s = 0; $s -lt $cfg.InitWaitSeconds; $s++) {
        Start-Sleep -Seconds 1
        if ($proc.HasExited) { throw "Instance exited during init (code $($proc.ExitCode)). See $outLog" }
    }

    & (Join-Path $PSScriptRoot 'Inject.ps1') -ProcessId $proc.Id -DllPath $dll
    $started += [pscustomobject]@{ Pid = $proc.Id; Proc = $proc; OutLog = $outLog; ErrLog = $errLog }
}

if ($NoWait) { Good "$($started.Count) instance(s) launched (not waiting for a port)"; return }

# --- wait for the servers to bind their game ports --------------------------------------
# The payload's own log (%TEMP%\HalcyonA2.log) cannot be read while a server is running:
# it is opened with _wfopen_s, which is exclusive, so the file is locked for the process
# lifetime. The bound UDP port is a better readiness signal anyway - it is the thing
# clients actually connect to, and it is what the DLL registers with the backend.
Info "waiting for game ports (up to $($cfg.ReadyTimeoutSeconds)s)"
$deadline = (Get-Date).AddSeconds($cfg.ReadyTimeoutSeconds)
$ready = @{}
while ((Get-Date) -lt $deadline -and $ready.Count -lt $started.Count) {
    foreach ($s in $started) {
        if ($ready[$s.Pid]) { continue }
        if ($s.Proc.HasExited) { throw "pid $($s.Pid) exited (code $($s.Proc.ExitCode)) before binding a port. See $($s.OutLog)" }
        $port = Get-NetUDPEndpoint -OwningProcess $s.Pid -ErrorAction SilentlyContinue |
                Where-Object { $_.LocalPort -ge 7777 -and $_.LocalPort -le 7877 } |
                Select-Object -Expand LocalPort -Unique | Sort-Object | Select-Object -First 1
        if ($port) {
            $ready[$s.Pid] = $port
            $name = if ($port -eq 7777) { 'HalcyonA2' } else { "HalcyonA2_$($port - 7777)" }
            Good "pid $($s.Pid): $name listening on UDP $port"
        }
    }
    if ($ready.Count -lt $started.Count) { Start-Sleep -Seconds 2 }
}

if ($ready.Count -lt $started.Count) {
    Warn2 "only $($ready.Count)/$($started.Count) instance(s) bound a game port within $($cfg.ReadyTimeoutSeconds)s."
    Warn2 "They are still running - check the logs below."
} else {
    Good "$($started.Count) server(s) up"
}

""
Info "payload log : $dllLog   (locked while a server runs; readable after it stops)"
Info "game logs   : $logDir"
Info "stop with   : .\Start-Server.ps1 -Stop      status: .\Start-Server.ps1 -Status"
