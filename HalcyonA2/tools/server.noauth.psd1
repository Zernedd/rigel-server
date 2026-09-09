@{
    # ---------------------------------------------------------------------------------
    # HalcyonA2 server settings - NO-AUTH TESTING PROFILE (see ExtraArgs at the bottom).
# Started by Start-Server-NoAuth.bat. For a real deployment use server.config.psd1.
    # Start-Server.ps1 -Config <other.psd1> loads a different file, so you can keep
    # e.g. server.local.psd1 and server.vps.psd1 side by side.
    # ---------------------------------------------------------------------------------

    # Which game build this server runs. Picks the payload built for it
    # (build\<GameBuild>\x64\<Configuration>\HalcyonA2.dll). Offsets are build-specific, so
    # GameBuild and GameExe must match - a 20996 payload in a 22284 exe just crashes.
    GameBuild = '22284'

    # Game binary. Relative paths resolve against the tools\ folder.
    #   22284: ..\..\Nov15\A2\Binaries\Win64\A2-Win64-Shipping.exe
    #   20996: ..\..\AnotherAxiom-A2-Rift\A2\Binaries\Win64\A2-Win64-Shipping.exe
    GameExe = '..\..\Nov15\A2\Binaries\Win64\A2-Win64-Shipping.exe'

    # Payload to inject. 'Release' or 'Debug' picks it out of build\<GameBuild>\x64\<cfg>\,
    # or give a full path to a specific DLL.
    Configuration = 'Release'
    DllPath       = ''

    # Seconds to let the engine come up before injecting. The DLL waits for a live
    # UWorld itself, so this only has to be long enough that injection is safe.
    InitWaitSeconds = 25

    # Extra time to wait for the DLL's console banner before reporting success.
    ReadyTimeoutSeconds = 120

    # ---------------------------------------------------------------------------------
    # Game arguments. The engine gets these verbatim; the DLL parses the -Dashboard*,
    # -RegisterIp and gamemode ones out of the command line itself.
    # ---------------------------------------------------------------------------------
    Args = @(
        '-nullrhi'        # no renderer: this is a headless server
        '-nohmd'          # never wait on a headset
        '-nosound'
        '-unattended'     # no modal dialogs
        '-nosplash'
        '-log'
    )

    # Station dashboard. Leave DeploymentId empty to let the backend mint one.
    DashboardApiUrl = ''
    DashboardApiKey = ''
    DeploymentId    = ''

    # Public IP clients connect to. Empty = the DLL auto-detects it (checkip.amazonaws.com
    # and friends). Set it if you are behind something that breaks detection.
    RegisterIp = '192.168.1.29'

    # Optional gamemode load, e.g. 'deathrun'. Empty = skip.
    LoadGamemode = ''
    GamemodeSlot = ''

    # Anything else you want appended, e.g. @('-QuietSims') for the ball-sim A/B test.
    # TESTING ONLY: -NoAuthGate makes AuthGateTick log-only. Without it the server kicks any
    # client that has not completed the dashboard auth handshake ("KICK no-auth-timeout")
    # about 30 s after it joins, which ends every local test. Never ship a server with this.
    ExtraArgs = @('-NoAuthGate')

    # Where to write per-run logs (relative to tools\).
    LogDir = 'logs'
}
