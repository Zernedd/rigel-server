# Runs ON THE VPS (copied there by Watch-ServerCrashes.ps1). Reads the allocator agent's log from byte offset -Since
# and reports every game server that went away WITHOUT being stopped on purpose, with the evidence for each: the
# payload's fault blocks for that pid (HalcyonA2.log, "FATAL ... pid=N"), the tail of that server's UE log, and the
# agent lines around it. Prints one JSON object: { offset, size, events: [...] }.
param([long]$Since = -1)
$ErrorActionPreference = 'Continue'
$agentLog = 'C:\Env\rigel-server\windows\logs\agent.log'
$payLog   = 'C:\Windows\Temp\HalcyonA2.log'
$ueLogs   = 'C:\Windows\System32\config\systemprofile\AppData\Local\A2\Saved\Logs'

function Read-From([string]$path, [long]$from, [long]$maxBytes) {
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $start = [Math]::Max($from, $fs.Length - $maxBytes)
        $fs.Seek($start, 'Begin') | Out-Null
        return @{ text = (New-Object IO.StreamReader($fs)).ReadToEnd(); size = $fs.Length }
    } finally { $fs.Close() }
}

$size = (Get-Item $agentLog).Length
if ($Since -lt 0 -or $Since -gt $size) {           # first run, or the log was rotated: start from here, report nothing
    [pscustomobject]@{ offset = $size; size = $size; events = @(); note = 'baseline' } | ConvertTo-Json -Compress
    return
}
$new = (Read-From $agentLog $Since 4MB).text
$lines = $new -split "`r?`n" | Where-Object { $_ }

# what happened to each pid in the new lines
$stopped = @{}; $gone = [ordered]@{}
foreach ($l in $lines) {
    if ($l -notmatch '^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)$') { continue }
    $t = $Matches[1]; $m = $Matches[2]
    if ($m -match 'kill pid (\d+): stopped') { $stopped[$Matches[1]] = $t; continue }
    if ($m -match '^pid (\d+) exited \(code (-?\d+)\)') { if (-not $gone.Contains($Matches[1])) { $gone[$Matches[1]] = @{ time = $t; code = $Matches[2]; how = 'exited' } }; continue }
    if ($m -match 'pid (\d+) exited during init \(code (-?\d+)\)') { if (-not $gone.Contains($Matches[1])) { $gone[$Matches[1]] = @{ time = $t; code = $Matches[2]; how = 'exited during start-up' } }; continue }
    if ($m -match 'probe pid (\d+): alive=False') { if (-not $gone.Contains($Matches[1])) { $gone[$Matches[1]] = @{ time = $t; code = ''; how = 'found dead by the watchdog' } }; continue }
}

$events = @()
if ($gone.Count -gt 0) {
    $pay = (Read-From $payLog 0 40MB).text -split "`n"
    foreach ($procId in $gone.Keys) {
        if ($stopped.ContainsKey($procId)) { continue }                 # stopped on purpose (dashboard / watchdog kill)
        $g = $gone[$procId]
        # the payload's fault blocks for this pid (first 3 blocks, 40 lines each)
        $blocks = @(); $cur = $null
        for ($i = 0; $i -lt $pay.Count -and $blocks.Count -lt 3; $i++) {
            $ln = $pay[$i]
            if ($ln -match "FATAL code=.* pid=$procId ") { $cur = New-Object System.Collections.Generic.List[string]; $cur.Add($ln.TrimEnd()); continue }
            if ($cur) {
                if ($ln -match '\[CRASH\]' -and $cur.Count -lt 40) { $cur.Add($ln.TrimEnd()) }
                if ($ln -match '\*\*\*\*\* end' -or $ln -notmatch '\[CRASH\]') { $blocks += ,($cur -join "`n"); $cur = $null }
            }
        }
        # its UE log: the one that stopped being written around the exit
        $ex = [datetime]::ParseExact($g.time, 'yyyy-MM-dd HH:mm:ss', $null)
        $ue = Get-ChildItem $ueLogs -Filter 'A2*.log' -EA SilentlyContinue |
              Where-Object { $_.LastWriteTime -gt $ex.AddMinutes(-12) -and $_.LastWriteTime -lt $ex.AddMinutes(2) }   # the watchdog can notice minutes late |
              Sort-Object { [Math]::Abs(($_.LastWriteTime - $ex).TotalSeconds) } | Select-Object -First 1
        $ueTail = ''
        if ($ue) {
            $ueTail = (Get-Content $ue.FullName -Tail 400 | Where-Object { $_ -notmatch 'SpherePrioritizer Successfully added|LogOnlineSession|VivoxCore|Verbose' } |
                       Select-Object -Last 120) -join "`n"
        }
        $ctx = ($lines | Where-Object { $_ -match "pid $procId\b|spinup|kill|connected to hub" } | Select-Object -Last 12) -join "`n"
        $events += [pscustomobject]@{
            pid = $procId; time = $g.time; code = $g.code; how = $g.how
            faults = $blocks; ueLog = $(if ($ue) { $ue.Name } else { '' }); ueTail = $ueTail; agent = $ctx
        }
    }
}
[pscustomobject]@{ offset = $size; size = $size; events = $events } | ConvertTo-Json -Depth 5 -Compress
