@{
    # =========================================================================
    #  Rigel GAME SERVER config (Windows).
    #  The backend runs on the Linux VPS; this is the A2 dedicated server that
    #  players actually connect to. It must run on Windows.
    #
    #  Everything deployment-specific is in ExtraArgs / the Dashboard* keys below.
    #  These MUST match the backend's .env (SERVER_API_KEY especially).
    # =========================================================================

    GameBuild = '22284'

    # The A2 Nov15 build is BUNDLED at windows\game\ — nothing external to fetch.
    # Relative paths resolve against this folder.
    GameExe = '.\game\A2\Binaries\Win64\A2-Win64-Shipping.exe'

    Configuration = 'Release'
    DllPath       = '.\HalcyonA2.dll'   # the payload shipped in this folder

    InitWaitSeconds     = 25
    ReadyTimeoutSeconds = 120

    Args = @(
        '-nullrhi'        # headless: no renderer
        '-nohmd'
        '-nosound'
        '-unattended'
        '-nosplash'
        '-log'
    )

    # ── Station dashboard — REQUIRED FOR IN-GAME QUESTS ──────────────────────
    # The client's Server_LoginToStationDashboard stores an org id on the
    # PlayerController; the DLL needs that org to initialise PlayerQuestComponent.
    # Blank here => [QUEST] logs "uninit comp ... org ''" forever and NO quests.
    # This is StationDb (port 78), fronted by TLS on 443.
    DashboardApiUrl = 'https://rigel.wwiggles.org'
    DashboardApiKey = 'halcyon-server-key'   # = A2StationDbServer.ServerMasterKey
    DeploymentId    = ''                     # blank = backend mints one

    # Public IP clients connect to. Blank = auto-detect (checkip/ipify/ifconfig).
    # If detection fails the server now REFUSES to register rather than publishing
    # a wrong address — set this explicitly if your box has no outbound :80.
    RegisterIp = ''

    LoadGamemode = ''
    GamemodeSlot = ''

    ExtraArgs = @(
        # ── Backend location. These used to be COMPILED INTO the DLL pointing at
        #    157.173.194.216 — a THIRD PARTY's server — so every station registered
        #    with, and pulled player quest data from, someone else's backend.
        #    These hosts are Cloudflare-fronted and answer on 443 ONLY (:90/:78
        #    time out), which is why an IP was used; the DLL now speaks TLS on 443.
        '-MothershipHost=rigel-ms.wwiggles.org'   # login/auth + quest fetch
        '-MothershipPort=443'
        '-BackendHost=rigel.wwiggles.org'         # register_server + player-count
        '-BackendPort=443'

        # Station name in the browser (base; extra instances get _1, _2...).
        # Was hardcoded "HalcyonA2" — which is why that station kept reappearing.
        '-ServerName=Rigel'

        # Shared secret. MUST equal SERVER_API_KEY in the backend .env.
        # ROTATE IT: the old value shipped as a string literal inside the DLL.
        # '-ServerApiKey=<64-hex>'

        # ── TESTING ONLY — remove before any public run ──────────────────────
        # '-NoAuthGate'    # auth gate logs only, never kicks
    )

    LogDir = 'logs'
}
