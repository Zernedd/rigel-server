<#
================================================================================
  Rigel - one-shot install for a WINDOWS VPS running BOTH the backend and the
  game server on the same box.

      Right-click PowerShell -> Run as administrator
      cd <this folder>
      Copy-Item .env.example .env ; notepad .env
      .\install.ps1

  Idempotent: re-run after editing .env. Flags:
      -NoBuild       skip dotnet publish (just re-apply config + restart)
      -NoFirewall    don't touch Windows Firewall
      -NoDashboardUi skip the Next.js dashboard UI (npm install + build + task)
      -Status        show what's running and exit
      -Uninstall     stop + remove the services (leaves data)

  WHY POWERSHELL AND NOT install.sh: the game server is A2-Win64-Shipping.exe with
  HalcyonA2.dll injected (WinHTTP, SEH/VEH, Get-NetUDPEndpoint) - Windows only.
  install.sh is for the Linux-backend-only split.
================================================================================
#>
[CmdletBinding()]
param(
    [switch] $NoBuild,
    [switch] $NoFirewall,
    [switch] $NoDashboardUi,
    [switch] $Status,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Ok  ($m) { Write-Host "[ok]   $m" -ForegroundColor Green }
function Inf ($m) { Write-Host "[info] $m" -ForegroundColor Cyan }
function Wrn ($m) { Write-Host "[warn] $m" -ForegroundColor Yellow }
function Die ($m) { Write-Host "[err]  $m" -ForegroundColor Red; exit 1 }

# -- admin check (urlacl + firewall + service install all need it) -------------
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Die "Run this from an ELEVATED PowerShell (Run as administrator)." }

$BackendDir  = Join-Path $PSScriptRoot 'backend'
$PublishDir  = Join-Path $PSScriptRoot 'run\backend'
$GameDir     = Join-Path $PSScriptRoot 'windows'
$DashUiDir   = Join-Path $BackendDir  'dashboard'
$TaskBackend = 'RigelBackend'
$TaskDashUi  = 'RigelDashboardUi'

# -- status / uninstall -------------------------------------------------------
if ($Status) {
    Inf "scheduled task:"
    Get-ScheduledTask -TaskName $TaskBackend,$TaskDashUi -ErrorAction SilentlyContinue |
        Select-Object TaskName,State | Format-Table -AutoSize
    Inf "backend process:"; Get-Process AUnrealFeatures.Ares -ErrorAction SilentlyContinue |
        Select-Object Id,ProcessName | Format-Table -AutoSize
    Inf "game servers:";    Get-Process A2-Win64-Shipping -ErrorAction SilentlyContinue |
        Select-Object Id,@{n='Started';e={$_.StartTime}} | Format-Table -AutoSize
    Inf "dashboard UI:";  Get-Process node -ErrorAction SilentlyContinue |
        Select-Object Id,ProcessName | Format-Table -AutoSize
    Inf "listening:"
    Get-NetTCPConnection -State Listen -EA SilentlyContinue |
        Where-Object LocalPort -in 50,78,80,90,3000,8080 |
        Select-Object LocalAddress,LocalPort -Unique | Format-Table -AutoSize
    Get-NetUDPEndpoint -EA SilentlyContinue | Where-Object { $_.LocalPort -ge 7777 -and $_.LocalPort -le 7787 } |
        Select-Object LocalAddress,LocalPort -Unique | Format-Table -AutoSize
    exit 0
}
if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskBackend -Confirm:$false -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskDashUi  -Confirm:$false -ErrorAction SilentlyContinue
    Get-Process AUnrealFeatures.Ares,A2-Win64-Shipping -EA SilentlyContinue | Stop-Process -Force
    # Only kill the node that is serving OUR dashboard, not every node on the box.
    Get-CimInstance Win32_Process -Filter "Name='node.exe'" -EA SilentlyContinue |
        Where-Object { $_.CommandLine -like '*next*start*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -EA SilentlyContinue }
    Ok "stopped + removed (data left in run\)"; exit 0
}

# -- read .env ----------------------------------------------------------------
if (-not (Test-Path .env)) { Die "No .env. Run:  Copy-Item .env.example .env ; notepad .env" }
$cfg = @{}
Get-Content .env | ForEach-Object {
    $l = $_.Trim()
    if ($l -and -not $l.StartsWith('#') -and $l.Contains('=')) {
        $k,$v = $l.Split('=',2)
        # Strip INLINE comments: .env.example has lines like "PORT_MOTHERSHIP=90     # login/auth",
        # and without this the value is the literal "90     # login/auth", which blows up the [int] casts
        # below with "Cannot convert value ... Input string was not in a correct format."
        # Split on whitespace-then-# so a '#' inside an actual value is left alone.
        $v = ($v -split '\s+#', 2)[0].Trim()
        # tolerate quoted values
        if ($v.Length -ge 2 -and (($v[0] -eq '"' -and $v[-1] -eq '"') -or ($v[0] -eq "'" -and $v[-1] -eq "'"))) {
            $v = $v.Substring(1, $v.Length - 2)
        }
        $cfg[$k.Trim()] = $v
    }
}
function C($k,$d=$null){ if($cfg.ContainsKey($k) -and $cfg[$k]){$cfg[$k]}else{$d} }

$PublicHost = C 'PUBLIC_HOST'
if (-not $PublicHost -or $PublicHost -eq 'YOUR-VPS-IP') {
    try { $PublicHost = (Invoke-RestMethod 'https://api.ipify.org?format=json' -TimeoutSec 10).ip
          Wrn "PUBLIC_HOST not set in .env; detected $PublicHost" } catch { Die "Set PUBLIC_HOST in .env" }
}
$ApiKey   = C 'SERVER_API_KEY'
$DashKey  = C 'DASHBOARD_API_KEY' 'halcyon-server-key'
$Build    = C 'BUILD_VERSION' '22284'
$P_MS     = [int](C 'PORT_MOTHERSHIP' 90)
$P_SD     = [int](C 'PORT_STATIONDB'  78)
$P_EOS    = [int](C 'PORT_EOS'        50)
$P_WS     = [int](C 'PORT_EOS_WS'     80)
$P_DASH   = [int](C 'PORT_DASHBOARD'  8080)
$P_DASHUI = [int](C 'PORT_DASHBOARD_UI' 3000)
$UdpFirst = [int](C 'GAME_UDP_FIRST'  7777)
$UdpLast  = [int](C 'GAME_UDP_LAST'   7787)
if ((C 'INSTALL_DASHBOARD_UI' '1') -ne '1') { $NoDashboardUi = $true }

if ((C 'MOTHERSHIP_INSECURE' '0') -eq '1') {
    Wrn "MOTHERSHIP_INSECURE=1 disables ALL identity checks (open auth bypass)."
    if ((Read-Host "Continue anyway? [y/N]") -ne 'y') { exit 1 }
}

# -- .NET ---------------------------------------------------------------------
$dotnet = (Get-Command dotnet -EA SilentlyContinue).Source
if (-not $dotnet) {
    Inf "installing .NET 6 SDK..."
    $ps1 = Join-Path $env:TEMP 'dotnet-install.ps1'
    Invoke-WebRequest 'https://dot.net/v1/dotnet-install.ps1' -OutFile $ps1 -UseBasicParsing
    & $ps1 -Channel 6.0 -InstallDir "$env:ProgramFiles\dotnet"
    $env:PATH = "$env:ProgramFiles\dotnet;$env:PATH"
    $dotnet = (Get-Command dotnet -EA SilentlyContinue).Source
}
if (-not $dotnet) { Die ".NET install failed" }
Ok "dotnet: $dotnet"

# -- apply .env into the compiled-in constants --------------------------------
# These are `const` in C#, read at BUILD time - without this you'd edit .env,
# deploy, and silently keep running the old baked-in secrets/app ids.
function Apply-EnvToSource {
    $ms = Join-Path $BackendDir 'src\AUnrealFeatures.AAMothership\MothershipServer.cs'
    $sd = Join-Path $BackendDir 'src\AUnrealFeatures.Ares\Servers\A2StationDbServer.cs'
    if (-not (Test-Path $ms) -or -not (Test-Path $sd)) { Wrn "backend sources missing; skipping"; return }

    # Build replacements by concatenation with an explicit quote char - nesting
    # escaped quotes inside -replace strings is where this gets unreadable/fragile.
    $q = [char]34
    $metaId  = C 'META_APP_ID'
    $metaSec = C 'META_APP_SECRET'
    $qpkg    = C 'QUEST_EXPECTED_PACKAGE'
    $qsha    = C 'QUEST_EXPECTED_CERT_SHA'

    $t = Get-Content $ms -Raw
    if ($ApiKey) { $t = $t -replace ('const string SERVER_API_KEY = ' + $q + '[^' + $q + ']*' + $q + ';'),
                                    ('const string SERVER_API_KEY = ' + $q + $ApiKey + $q + ';') }
    if ($metaId) { $t = $t -replace ('const string META_APP_ID     = ' + $q + '[^' + $q + ']*' + $q + ';'),
                                    ('const string META_APP_ID     = ' + $q + $metaId + $q + ';') }
    if ($metaSec){ $t = $t -replace ('const string META_APP_SECRET = ' + $q + '[^' + $q + ']*' + $q + ';'),
                                    ('const string META_APP_SECRET = ' + $q + $metaSec + $q + ';') }
    if ($qpkg)   { $t = $t -replace ('(INSECURE_TESTING \? ' + $q + $q + ' : )' + $q + 'org\.[^' + $q + ']*' + $q),
                                    ('$1' + $q + $qpkg + $q) }
    if ($qsha)   { $t = $t -replace ('(INSECURE_TESTING \? ' + $q + $q + ' : )' + $q + '[0-9a-f]{64}' + $q),
                                    ('$1' + $q + $qsha + $q) }
    Set-Content $ms $t -NoNewline

    $t = Get-Content $sd -Raw
    $t = $t -replace ('private const string BuildVersion = ' + $q + '[^' + $q + ']*' + $q + ';'),
                     ('private const string BuildVersion = ' + $q + $Build + $q + ';')
    $t = $t -replace 'private const int    BuildIdNum   = \d+;', ('private const int    BuildIdNum   = ' + $Build + ';')
    Set-Content $sd $t -NoNewline

    # verify - a silently-failed replace is worse than none
    if ((Get-Content $ms -Raw).Contains('SERVER_API_KEY = ' + $q + $ApiKey + $q)) { Ok 'applied SERVER_API_KEY' }
    else { Wrn 'SERVER_API_KEY NOT applied' }
    if ((Get-Content $sd -Raw).Contains('BuildVersion = ' + $q + $Build + $q)) { Ok "applied BUILD_VERSION=$Build" }
    else { Wrn 'BUILD_VERSION NOT applied' }
    if ($metaSec -eq 'CHANGE_ME') { Wrn 'META_APP_SECRET is still CHANGE_ME - Quest login attestation WILL fail' }

    # never ship a seeded ghost station (this once put a fake OCE_1 at 127.0.0.1
    # into every player's browser), and drop stale persisted sessions
    $mm = Join-Path $BackendDir 'src\AUnrealFeatures.EOSSDK\AUnrealFeatures.EOSSDK\Responses\matchmaking.json'
    if (Test-Path $mm) {
        $j = Get-Content $mm -Raw
        if ($j -match '"sessions"\s*:\s*\[\s*\{') {
            Wrn "matchmaking.json seeds a fake session - emptying"
            $empty = @('{','    "count": 0,','    "sessions": []','}') -join "`r`n"
            Set-Content $mm $empty
        }
    }
    Remove-Item (Join-Path $PublishDir 'sessions.json'),(Join-Path $PublishDir 'lobbies.json') -EA SilentlyContinue
}

if (-not $NoBuild) {
    Inf "applying .env into backend sources..."
    Apply-EnvToSource
    Inf "publishing backend (takes a minute)..."
    & $dotnet publish (Join-Path $BackendDir 'src\AUnrealFeatures.Ares') -c Release -o $PublishDir --nologo -v q
    if ($LASTEXITCODE -ne 0) { Die "dotnet publish failed" }
    Ok "published -> $PublishDir"
}

# -- low-port reservations (50/78/80/90 are privileged on Windows) ------------
Inf "reserving low ports (urlacl)..."
$who = "$env:USERDOMAIN\$env:USERNAME"
foreach ($p in @($P_EOS,$P_SD,$P_WS,$P_MS,$P_DASH)) {
    netsh http delete urlacl url="http://+:$p/" 2>&1 | Out-Null
    netsh http add urlacl url="http://+:$p/" user="$who" 2>&1 | Out-Null
}
Ok "urlacl set for $P_EOS,$P_SD,$P_WS,$P_MS,$P_DASH"

# -- firewall -----------------------------------------------------------------
# NOTE: Contabo ALSO has an external firewall in the customer panel; the Windows
# rules below are not enough on their own. See docs\CONTABO-PORTS.md.
if (-not $NoFirewall -and (C 'MANAGE_FIREWALL' '1') -eq '1') {
    Inf "configuring Windows Firewall..."
    foreach ($n in 'Rigel Backend (TCP)','Rigel Game (UDP)','Rigel Dashboard (TCP)') {
        Remove-NetFirewallRule -DisplayName $n -EA SilentlyContinue
    }
    New-NetFirewallRule -DisplayName 'Rigel Backend (TCP)' -Direction Inbound -Action Allow `
        -Protocol TCP -LocalPort $P_EOS,$P_SD,$P_WS,$P_MS -Profile Any | Out-Null
    New-NetFirewallRule -DisplayName 'Rigel Game (UDP)' -Direction Inbound -Action Allow `
        -Protocol UDP -LocalPort "$UdpFirst-$UdpLast" -Profile Any | Out-Null
    Ok "opened TCP $P_EOS,$P_SD,$P_WS,$P_MS and UDP $UdpFirst-$UdpLast"
    Wrn "dashboard :$P_DASH and orchestrator 9095/9100 deliberately NOT opened."
    Wrn "  Dashboard auth is DISABLED (Enforce=false) - reach it over an SSH/RDP tunnel. See docs\DASHBOARD.md."
}

# -- cloudflared tunnel (reverse proxy) --------------------------------------
# The rigel-* hostnames are Cloudflare-fronted and answer on 443 ONLY. The tunnel
# must run on the SAME box as the backend; it reaches it over loopback, so no
# inbound service ports are needed. Moving the tunnel here does NOT touch DNS -
# the routes already point at this tunnel, we are only changing where it runs.
$CfTunnel = C 'CLOUDFLARED_TUNNEL'
$CfCreds  = C 'CLOUDFLARED_CREDENTIALS'
# Base domain for the rigel-* hostnames. DOMAIN in .env is normally only used for the
# Caddy/Let's-Encrypt path, but the tunnel needs it too; default to the project's domain.
$DOMAIN0  = C 'DOMAIN' 'wwiggles.org'
if ($CfTunnel) {
    Inf "cloudflared: tunnel '$CfTunnel' -> rigel-ms/rigel/rigel-eos.$DOMAIN0"
    if (-not $CfCreds -or -not (Test-Path $CfCreds)) {
        Wrn "CLOUDFLARED_TUNNEL='$CfTunnel' but CLOUDFLARED_CREDENTIALS is missing/not found."
        Wrn "  Copy %USERPROFILE%\.cloudflared\<TUNNEL-ID>.json from your local PC to this box."
        Wrn "  Skipping tunnel setup."
    } else {
        $cfDir = Join-Path $PSScriptRoot 'run\cloudflared'
        New-Item -ItemType Directory -Force -Path $cfDir | Out-Null
        $cfExe = Join-Path $cfDir 'cloudflared.exe'
        if (-not (Test-Path $cfExe)) {
            Inf "downloading cloudflared..."
            try {
                Invoke-WebRequest 'https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe' `
                    -OutFile $cfExe -UseBasicParsing
            } catch { Wrn "cloudflared download failed: $($_.Exception.Message)" }
        }
        if (Test-Path $cfExe) {
            # keep the credentials next to the config so the service (SYSTEM) can read them
            $cfCredDst = Join-Path $cfDir ([IO.Path]::GetFileName($CfCreds))
            Copy-Item $CfCreds $cfCredDst -Force
            $cfYml = Join-Path $cfDir 'config.yml'
            @(
                "tunnel: $CfTunnel"
                "credentials-file: $cfCredDst"
                ""
                "ingress:"
                "  - hostname: rigel-ms.$($DOMAIN0)"
                "    service: http://127.0.0.1:$P_MS"
                "  - hostname: rigel.$($DOMAIN0)"
                "    service: http://127.0.0.1:$P_SD"
                "  - hostname: rigel-eos.$($DOMAIN0)"
                "    service: http://127.0.0.1:$P_EOS"
                "  - service: http_status:404"
            ) -join "`r`n" | Set-Content $cfYml -Encoding ASCII
            Ok "wrote $cfYml"

            # (re)install as a Windows service so it survives reboot/logoff
            & $cfExe service uninstall 2>&1 | Out-Null
            & $cfExe --config $cfYml service install 2>&1 | Out-Null
            Start-Sleep -Seconds 2
            $svc = Get-Service cloudflared -EA SilentlyContinue
            if ($svc) {
                Start-Service cloudflared -EA SilentlyContinue
                Start-Sleep -Seconds 3
                $svc = Get-Service cloudflared -EA SilentlyContinue
                Ok "cloudflared service: $($svc.Status)"
            } else {
                Wrn "cloudflared service not registered; run manually to debug:"
                Wrn "  `"$cfExe`" --config `"$cfYml`" tunnel run $CfTunnel"
            }
            Wrn "STOP the tunnel on your LOCAL PC now, or cloudflared will load-balance"
            Wrn "  between both replicas and half the requests will hit the old backend."
        }
    }
}

# -- run the backend (scheduled task = survives logoff, autostarts on boot) ---
Inf "registering '$TaskBackend' to run at startup..."
Unregister-ScheduledTask -TaskName $TaskBackend -Confirm:$false -EA SilentlyContinue
$exe = Join-Path $PublishDir 'AUnrealFeatures.Ares.exe'
# NOTE: -Argument rejects an empty string ("The argument is null or empty"), so only pass it when
# we actually launch via `dotnet <dll>`. When the apphost .exe exists it takes no arguments.
if (Test-Path $exe) {
    $act = New-ScheduledTaskAction -Execute $exe -WorkingDirectory $PublishDir
} else {
    $dll = Join-Path $PublishDir 'AUnrealFeatures.Ares.dll'
    if (-not (Test-Path $dll)) { Die "neither AUnrealFeatures.Ares.exe nor .dll found in $PublishDir - did publish succeed?" }
    $act = New-ScheduledTaskAction -Execute $dotnet -Argument ('"' + $dll + '"') -WorkingDirectory $PublishDir
}
$trg = New-ScheduledTaskTrigger -AtStartup
$pri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskBackend -Action $act -Trigger $trg -Principal $pri -Settings $set | Out-Null

Get-Process AUnrealFeatures.Ares -EA SilentlyContinue | Stop-Process -Force
Start-ScheduledTask -TaskName $TaskBackend
Start-Sleep -Seconds 6

$listening = Get-NetTCPConnection -State Listen -EA SilentlyContinue |
             Where-Object LocalPort -in $P_MS,$P_SD,$P_EOS,$P_DASH |
             Select-Object -Expand LocalPort -Unique | Sort-Object
if ($listening) { Ok "backend listening on: $($listening -join ', ')" }
else { Wrn "backend not listening yet - check: Get-Content '$PublishDir\logs\*.txt' -Tail 40" }

# -- root certificates for the game server's HTTPS calls ----------------------
# The game build ships no cacert.pem, so UE's libcurl verifies against the WINDOWS
# root store. A fresh Windows Server image carries a minimal root set and fetches the
# rest on demand from ctldl.windowsupdate.com; when that is blocked (common on a
# locked-down VPS) an https call to AWS fails with
#   libcurl error 60 ... unable to get local issuer certificate
# and the game's HTTP completion handler then dereferences the null response object
# (A2-Win64-Shipping.exe +0x46C56C1) and takes the whole server down ~40s after boot.
# A dev PC never hits this because its root store is fully populated - which is exactly
# why "it runs on my PC but crashes on the server".
$certDir = Join-Path $GameDir 'certs'
if (Test-Path $certDir) {
    foreach ($cf in Get-ChildItem $certDir -Filter '*.cer' -EA SilentlyContinue) {
        try {
            $c  = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cf.FullName
            $have = Get-ChildItem Cert:\LocalMachine\Root -EA SilentlyContinue |
                    Where-Object Thumbprint -eq $c.Thumbprint
            if ($have) { Inf "root CA already trusted: $($cf.BaseName)" }
            else {
                Import-Certificate -FilePath $cf.FullName -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
                Ok "installed root CA: $($cf.BaseName) ($($c.Thumbprint))"
            }
        } catch { Wrn "could not install $($cf.Name): $($_.Exception.Message)" }
    }
}
# Also re-enable Windows' automatic root updates if a policy turned them off, so future
# roots arrive on their own rather than needing another manual import.
$rootPol = 'HKLM:\SOFTWARE\Policies\Microsoft\SystemCertificates\AuthRoot'
if (Test-Path $rootPol) {
    $dis = (Get-ItemProperty $rootPol -Name DisableRootAutoUpdate -EA SilentlyContinue).DisableRootAutoUpdate
    if ($dis -eq 1) {
        Set-ItemProperty $rootPol -Name DisableRootAutoUpdate -Value 0
        Ok "re-enabled Windows automatic root certificate updates"
    }
}

# -- Next.js dashboard UI (optional, nicer front-end for the same :8080 API) --
# The backend already serves a static UI on :8080. This is the richer React one in
# backend\dashboard (Overview / Users / Stations / Deployments / Events / Telemetry /
# WS Monitor). It is a PROXY only: next.config.ts rewrites /api,/auth -> :$P_DASH and
# /game -> :$P_SD, so it holds no state and needs no secrets of its own.
$DashUiReady = $false
if ($NoDashboardUi) {
    Inf "dashboard UI skipped (-NoDashboardUi / INSTALL_DASHBOARD_UI=0)"
} elseif (-not (Test-Path (Join-Path $DashUiDir 'package.json'))) {
    Wrn "no backend\dashboard\package.json - skipping the Next.js UI"
} else {
    $npm  = (Get-Command npm.cmd -EA SilentlyContinue).Source
    $node = (Get-Command node.exe -EA SilentlyContinue).Source
    if (-not $npm -or -not $node) {
        Wrn "Node.js not found - skipping the Next.js dashboard UI."
        Wrn "  Install it, then re-run this script:   winget install OpenJS.NodeJS.LTS"
        Wrn "  (open a NEW shell afterwards so PATH picks it up)"
        Wrn "  The backend's own UI on :$P_DASH works without Node."
    } else {
        # Next 16 needs Node 20+. Building on 18 fails with confusing syntax errors.
        $nodeMajor = 0
        if ((& $node -v) -match '^v(\d+)') { $nodeMajor = [int]$Matches[1] }
        if ($nodeMajor -lt 20) {
            Wrn "Node $(& $node -v) is too old for Next 16 (needs >=20) - skipping the dashboard UI."
        } else {
            Inf "building the dashboard UI (Node $(& $node -v); first run takes a few minutes)..."
            Push-Location $DashUiDir
            try {
                # `npm ci` is reproducible but hard-fails if the lockfile drifts; fall back to install.
                & $npm 'ci' '--no-audit' '--no-fund' 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) { & $npm 'install' '--no-audit' '--no-fund' 2>&1 | Out-Null }
                if ($LASTEXITCODE -ne 0) { throw "npm install failed (exit $LASTEXITCODE)" }
                $env:BACKEND_HOST      = '127.0.0.1'
                $env:DASHBOARD_API_PORT = "$P_DASH"
                $env:STATION_API_PORT   = "$P_SD"
                & $npm 'run' 'build'
                if ($LASTEXITCODE -ne 0) { throw "next build failed (exit $LASTEXITCODE)" }
                $DashUiReady = $true
            } catch {
                Wrn "dashboard UI build failed: $($_.Exception.Message)"
                Wrn "  the backend UI on :$P_DASH is unaffected; retry with .\install.ps1 -NoBuild"
            } finally { Pop-Location }
        }
    }
}

if ($DashUiReady) {
    # A scheduled task cannot carry environment variables, and `npm` is a .cmd shim that
    # behaves badly under SYSTEM - so launch node's next binary directly from a wrapper
    # that sets the env first.
    $wrapper = Join-Path $PSScriptRoot 'run\dashboard-ui.cmd'
    New-Item -ItemType Directory -Path (Split-Path $wrapper) -Force | Out-Null
    @"
@echo off
REM Generated by install.ps1 - re-run it to regenerate. Serves the Next.js dashboard UI.
set BACKEND_HOST=127.0.0.1
set DASHBOARD_API_PORT=$P_DASH
set STATION_API_PORT=$P_SD
set PORT=$P_DASHUI
set HOSTNAME=127.0.0.1
cd /d "$DashUiDir"
"$node" "node_modules\next\dist\bin\next" start -p $P_DASHUI -H 127.0.0.1
"@ | Set-Content -LiteralPath $wrapper -Encoding ASCII

    Inf "registering '$TaskDashUi' to run at startup..."
    Unregister-ScheduledTask -TaskName $TaskDashUi -Confirm:$false -EA SilentlyContinue
    $dAct = New-ScheduledTaskAction -Execute $wrapper -WorkingDirectory $DashUiDir
    $dTrg = New-ScheduledTaskTrigger -AtStartup
    $dPri = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $dSet = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                                         -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $TaskDashUi -Action $dAct -Trigger $dTrg -Principal $dPri -Settings $dSet | Out-Null

    Get-CimInstance Win32_Process -Filter "Name='node.exe'" -EA SilentlyContinue |
        Where-Object { $_.CommandLine -like '*next*start*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -EA SilentlyContinue }
    Start-ScheduledTask -TaskName $TaskDashUi
    Start-Sleep -Seconds 8
    # -H 127.0.0.1 binds LOOPBACK ONLY. Auth is off (Enforce=false / AUTH_ENFORCE=false),
    # so this must never be reachable from the internet - reach it over RDP or an SSH tunnel.
    if (Get-NetTCPConnection -State Listen -LocalPort $P_DASHUI -EA SilentlyContinue) {
        Ok "dashboard UI listening on 127.0.0.1:$P_DASHUI (loopback only)"
    } else {
        Wrn "dashboard UI did not bind :$P_DASHUI yet - check: Get-ScheduledTask $TaskDashUi | Get-ScheduledTaskInfo"
    }
}

# -- pair the GAME SERVER to this same box -----------------------------------
# Both roles are on one machine, so the game server talks to the backend over
# LOOPBACK (plain HTTP, no TLS, no Cloudflare round-trip). Only RegisterIp must
# be the PUBLIC address, because that is what clients are told to connect to.
$gameCfg = Join-Path $GameDir 'server.config.psd1'
if (Test-Path $gameCfg) {
    $g = Get-Content $gameCfg -Raw
    $g = $g -replace "DashboardApiUrl = '[^']*'", "DashboardApiUrl = 'http://127.0.0.1:$P_SD'"
    $g = $g -replace "RegisterIp = '[^']*'",      "RegisterIp = '$PublicHost'"
    $g = $g -replace "'-MothershipHost=[^']*'",   "'-MothershipHost=127.0.0.1'"
    $g = $g -replace "'-MothershipPort=[^']*'",   "'-MothershipPort=$P_MS'"
    $g = $g -replace "'-BackendHost=[^']*'",      "'-BackendHost=127.0.0.1'"
    $g = $g -replace "'-BackendPort=[^']*'",      "'-BackendPort=$P_SD'"
    if ($ApiKey) {
        if ($g -match "'-ServerApiKey=") { $g = $g -replace "'-ServerApiKey=[^']*'", "'-ServerApiKey=$ApiKey'" }
        else { $g = $g -replace "(\s*)'-BackendPort=[^']*'", "`$0`$1'-ServerApiKey=$ApiKey'" }
    }
    Set-Content $gameCfg $g -NoNewline
    Ok "paired windows\server.config.psd1 to this box (backend over 127.0.0.1, RegisterIp=$PublicHost)"
}

@"

--------------------------------------------------------------------------
 Rigel installed (backend + game server on this one Windows box).

   backend status : .\install.ps1 -Status
   backend logs   : $PublishDir\logs\
   restart backend: Restart-ScheduledTask -TaskName $TaskBackend
   restart dash UI: Restart-ScheduledTask -TaskName $TaskDashUi

 START A GAME SERVER (registers itself and appears in the browser):
   cd windows
   .\Start-Server.ps1

 Clients reach you at : $PublicHost  (UDP $UdpFirst-$UdpLast must be open)

 DASHBOARDS (both LOCAL ONLY - auth is off, do not expose either port):
   Full UI (React)    : http://127.0.0.1:$P_DASHUI $(if(-not $DashUiReady){'  <-- NOT RUNNING, see warnings above'})
   Basic UI + API     : http://127.0.0.1:$P_DASH
   From your own PC   : ssh -L ${P_DASHUI}:127.0.0.1:$P_DASHUI -L ${P_DASH}:127.0.0.1:$P_DASH Administrator@$PublicHost
                        ...or just open it in a browser over RDP.

 Contabo has an EXTERNAL firewall in the customer panel too - the Windows
 rules above are not enough on their own. See docs\CONTABO-PORTS.md.
--------------------------------------------------------------------------
"@ | Write-Host
