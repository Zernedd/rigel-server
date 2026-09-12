<#
.SYNOPSIS
  Halcyon allocator agent: lets the dashboard's "Spin Up" launch game servers on this box.

.DESCRIPTION
  Connects to the backend's HalcyonSocket agent hub (TCP 9100, newline-delimited JSON) and
  announces how many game servers this box may run. When the dashboard asks for a server, the
  hub relays {"type":"spinup","id","map","region","args"}; this agent launches
  A2-Win64-Shipping.exe with the same arguments Start-Server.ps1 uses (server.config.psd1) plus
  the spin-up args (which carry the -DashboardDeploymentId the backend minted), injects
  HalcyonA2.dll once the engine is up, and reports back. The server then self-registers and
  shows up in the browser. UE picks the next free game port (7777, 7778, ...).

  Messages sent: hello {box, capacity, running}, heartbeat {running} every 30s,
  spunup {id, pid, status, msg}, exited {pid, code, running}, probe_result {pid, alive}.
  Messages handled: spinup {id, map, region, args}, probe {pid}, kill {pid, reason}.

  probe/kill serve the backend's watchdog (ServerWatchdog). It replaces a server only when it is
  CERTAIN the old one is down, and this box is the only thing that can supply that certainty: the
  watchdog will not act on silence alone, it asks here whether the process still exists. kill is used
  before every replacement, because a FROZEN server is still alive and still owns its game port.

  Capacity counts EVERY A2 server process on the box, including the one the RigelGameServer task
  starts. One server uses roughly 2 cores and ~1 GB, so the default of 2 suits a 6-core VPS.

  The hub port has no authentication: keep 9100 (and 9095) blocked from the internet.

.EXAMPLE
  .\HalcyonAgent.ps1                       # hub 127.0.0.1:9100, capacity 2
  .\HalcyonAgent.ps1 -Capacity 3 -Box vps1
#>
param(
    [string] $HubHost  = '127.0.0.1',
    [int]    $HubPort  = 9100,
    [int]    $Capacity = 2,
    [string] $Box      = $env:COMPUTERNAME,
    [string] $Config   = 'server.config.psd1'
)

$ErrorActionPreference = 'Stop'
$procName = 'A2-Win64-Shipping'
$logDir   = Join-Path $PSScriptRoot 'logs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$agentLog = Join-Path $logDir 'agent.log'

function Log($m) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"
    Write-Host $line
    Add-Content -Path $agentLog -Value $line
}

function Resolve-FromHere([string] $p) {
    if ([string]::IsNullOrWhiteSpace($p)) { return $null }
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $p))
}

function Running-Count { @(Get-Process -Name $procName -ErrorAction SilentlyContinue).Count }

# Base arguments: identical to Start-Server.ps1, except the config's fixed DeploymentId -- a spun-up
# server gets the id the backend minted (it arrives in the spin-up args).
function Base-Args($cfg) {
    $a = @($cfg.Args)
    if ($cfg.DashboardApiUrl) { $a += "-DashboardApiUrl=$($cfg.DashboardApiUrl)" }
    if ($cfg.DashboardApiKey) { $a += "-DashboardApiKey=$($cfg.DashboardApiKey)" }
    if ($cfg.RegisterIp)      { $a += "-RegisterIp=$($cfg.RegisterIp)" }
    if ($cfg.GamemodeSlot)    { $a += "-GamemodeSlot=$($cfg.GamemodeSlot)" }
    if ($cfg.ExtraArgs)       { $a += @($cfg.ExtraArgs) }
    return ,$a
}

# Spin-up args come from the dashboard. Accept only plain "-Flag" / "-Key=Value" tokens so nothing
# can smuggle quoting or a second command into the launch.
function Safe-Args([string] $s) {
    $out = @()
    foreach ($tok in ($s -split '\s+')) {
        if ([string]::IsNullOrWhiteSpace($tok)) { continue }
        if ($tok -match '^-[A-Za-z][A-Za-z0-9_.]*(=[A-Za-z0-9_.:/\-,]*)?$') { $out += $tok }
        else { Log "ignored unsafe spin-up arg: $tok" }
    }
    return ,$out
}

function Send($writer, $obj) {
    $writer.WriteLine(($obj | ConvertTo-Json -Compress))
    $writer.Flush()
}

$configPath = Resolve-FromHere $Config
Log "agent starting: box=$Box capacity=$Capacity hub=${HubHost}:$HubPort config=$configPath"

while ($true) {
    $client = $null
    try {
        $cfg = Import-PowerShellDataFile -LiteralPath $configPath
        $exe = Resolve-FromHere $cfg.GameExe
        $dll = if ($cfg.DllPath) { Resolve-FromHere $cfg.DllPath } else { Resolve-FromHere "..\build\$($cfg.GameBuild)\x64\$($cfg.Configuration)\HalcyonA2.dll" }
        $initWait = if ($cfg.InitWaitSeconds) { [int]$cfg.InitWaitSeconds } else { 90 }

        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect($HubHost, $HubPort)
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
        Send $writer @{ type = 'hello'; box = $Box; capacity = $Capacity; running = (Running-Count) }
        Log "connected to hub; running=$(Running-Count)"

        $pending   = @{}   # pid -> @{ Proc; Id; InjectAt }  (launched, waiting to inject)
        $ours      = @{}   # pid -> Process                 (launched by this agent, injected)
        $nextBeat  = (Get-Date).AddSeconds(30)
        $readTask  = $reader.ReadLineAsync()

        while ($client.Connected) {
            # 1. messages from the hub
            if ($readTask.Wait(1000)) {
                $line = $readTask.Result
                if ($null -eq $line) { throw "hub closed the connection" }
                $readTask = $reader.ReadLineAsync()
                $msg = $null
                try { $msg = $line | ConvertFrom-Json } catch { Log "bad message: $line" }
                # The watchdog asks whether a pid is still running. This is the answer it trusts over
                # silence: only the box can tell a dead server apart from a dead network.
                if ($msg -and $msg.type -eq 'probe') {
                    $p = Get-Process -Id ([int]$msg.pid) -ErrorAction SilentlyContinue
                    $alive = [bool]($p -and -not $p.HasExited -and $p.ProcessName -eq $procName)
                    Send $writer @{ type = 'probe_result'; pid = "$($msg.pid)"; alive = $alive }
                    Log "probe pid $($msg.pid): alive=$alive"
                }
                # Force-stop a server the watchdog has ruled down. A FROZEN one is still alive and still
                # owns its game port, so it has to go before the replacement launches or the box ends up
                # running two servers for one station.
                elseif ($msg -and $msg.type -eq 'kill') {
                    $targetId = [int]$msg.pid
                    $p = Get-Process -Id $targetId -ErrorAction SilentlyContinue
                    if (-not $p -or $p.ProcessName -ne $procName) {
                        Log "kill pid ${targetId}: already gone (or not a game server) -- nothing to do"
                        Send $writer @{ type = 'exited'; pid = "$targetId"; code = 'gone'; running = (Running-Count) }
                    }
                    else {
                        try {
                            Stop-Process -Id $targetId -Force -ErrorAction Stop
                            Start-Sleep -Milliseconds 1500
                            Log "kill pid ${targetId}: stopped ($($msg.reason))"
                        }
                        catch { Log "kill pid ${targetId}: FAILED -- $($_.Exception.Message)" }
                        $pending.Remove($targetId); $ours.Remove($targetId)
                        Send $writer @{ type = 'exited'; pid = "$targetId"; code = 'killed'; running = (Running-Count) }
                    }
                }
                elseif ($msg -and $msg.type -eq 'spinup') {
                    $running = Running-Count
                    if ($running -ge $Capacity) {
                        Log "spinup $($msg.id) refused: box full ($running/$Capacity)"
                        Send $writer @{ type = 'spunup'; id = $msg.id; pid = ''; status = 'error'; msg = "box full ($running/$Capacity)" }
                    }
                    else {
                        $launchArgs = Base-Args $cfg
                        if ($msg.map) { $launchArgs += "-LoadGamemode=$($msg.map)" }
                        $launchArgs += Safe-Args ([string]$msg.args)
                        $stamp  = Get-Date -Format yyyyMMdd-HHmmss
                        $outLog = Join-Path $logDir "spinup-$stamp.out.log"
                        $errLog = Join-Path $logDir "spinup-$stamp.err.log"
                        $proc = Start-Process -FilePath $exe -ArgumentList $launchArgs -WorkingDirectory (Split-Path -Parent $exe) `
                                              -PassThru -RedirectStandardOutput $outLog -RedirectStandardError $errLog
                        $pending[$proc.Id] = @{ Proc = $proc; Id = $msg.id; InjectAt = (Get-Date).AddSeconds($initWait) }
                        $shown = ($launchArgs -join ' ') -replace '(?i)(ApiKey=)\S+', '$1***'
                        Log "spinup $($msg.id): launched pid $($proc.Id) (inject in ${initWait}s) args: $shown"
                    }
                }
            }

            # 2. inject launched servers once the engine has had time to come up
            foreach ($procId in @($pending.Keys)) {
                $p = $pending[$procId]
                if ($p.Proc.HasExited) {
                    Log "spinup $($p.Id): pid $procId exited during init (code $($p.Proc.ExitCode))"
                    Send $writer @{ type = 'spunup'; id = $p.Id; pid = "$procId"; status = 'error'; msg = "exited during init (code $($p.Proc.ExitCode))" }
                    $pending.Remove($procId)
                    continue
                }
                if ((Get-Date) -lt $p.InjectAt) { continue }
                try {
                    & (Join-Path $PSScriptRoot 'Inject.ps1') -ProcessId $procId -DllPath $dll | Out-Null
                    $ours[$procId] = $p.Proc
                    Log "spinup $($p.Id): injected pid $procId"
                    Send $writer @{ type = 'spunup'; id = $p.Id; pid = "$procId"; status = 'ok'; msg = 'launched and injected; it self-registers once up' }
                }
                catch {
                    Log "spinup $($p.Id): inject failed for pid ${procId}: $($_.Exception.Message)"
                    try { Stop-Process -Id $procId -Force } catch {}
                    Send $writer @{ type = 'spunup'; id = $p.Id; pid = "$procId"; status = 'error'; msg = "inject failed: $($_.Exception.Message)" }
                }
                $pending.Remove($procId)
            }

            # 3. servers we launched that have stopped
            foreach ($procId in @($ours.Keys)) {
                if (-not $ours[$procId].HasExited) { continue }
                $code = $ours[$procId].ExitCode
                $ours.Remove($procId)
                Log "pid $procId exited (code $code)"
                Send $writer @{ type = 'exited'; pid = "$procId"; code = "$code"; running = (Running-Count) }
            }

            # 4. heartbeat
            if ((Get-Date) -ge $nextBeat) {
                Send $writer @{ type = 'heartbeat'; running = (Running-Count) }
                $nextBeat = (Get-Date).AddSeconds(30)
            }
        }
    }
    catch {
        Log "hub connection: $($_.Exception.Message) -- retrying in 10s"
    }
    finally {
        if ($client) { try { $client.Close() } catch {} }
    }
    Start-Sleep -Seconds 10
}
