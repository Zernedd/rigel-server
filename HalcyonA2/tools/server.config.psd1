@{
    # ---------------------------------------------------------------------------------
    # HalcyonA2 server settings. Edit here, not in Start-Server.ps1.
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

    # Station dashboard. REQUIRED FOR IN-GAME QUESTS (and for the station info-boards).
    # Chain: the client's Server_LoginToStationDashboard stores an org id on the PlayerController
    # (+0xA30); the DLL reads it to init each PlayerQuestComponent. With these blank there is no
    # dashboard login -> org is "" -> [QUEST] logs "uninit comp ... org ''" forever and NO quests.
    # DashboardApiUrl must point at the StationDb service (A2StationDbServer, PORT 78), which serves
    # GET /v1/deployments/{id}?include_station_config=true.
    # DashboardApiKey must equal A2StationDbServer.ServerMasterKey ("halcyon-server-key").
    # Leave DeploymentId empty to let the backend mint one.
    DashboardApiUrl = 'https://rigel.wwiggles.org'
    DashboardApiKey = 'halcyon-server-key'
    DeploymentId    = ''

    # Public IP clients connect to. Empty = the DLL auto-detects it (checkip.amazonaws.com
    # and friends). Set it if you are behind something that breaks detection.
    RegisterIp = ''

    # Optional gamemode load, e.g. 'deathrun'. Empty = skip.
    LoadGamemode = ''
    GamemodeSlot = ''

    # Anything else you want appended, e.g. @('-QuietSims') for the ball-sim A/B test.
    # -NoAuthGate: auth gate is LOG-ONLY (no kicks). TESTING ONLY — remove before any public/prod run.
    #
    # Backend location + shared secret. These used to be COMPILED INTO the DLL and pointed at
    # 157.173.194.216, which is NOT this project's server — every station was registering with, and
    # pulling quests from, a third party's backend. Now they target the real domains. Those hosts are
    # Cloudflare-fronted and answer on 443 ONLY (:90/:78 time out), so the DLL now speaks TLS on 443.
    ExtraArgs = @(
        '-NoAuthGate'
        '-ServerName=Rigel'                       # station name in the browser (was hardcoded HalcyonA2)
        '-MothershipHost=rigel-ms.wwiggles.org'   # login/auth + quest fetch
        '-MothershipPort=443'
        '-BackendHost=rigel.wwiggles.org'         # register_server + player-count heartbeat
        '-BackendPort=443'
    )

    # Where to write per-run logs (relative to tools\).
    LogDir = 'logs'
}
