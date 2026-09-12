@{
    # ---------------------------------------------------------------------------------
    # COSMETIC ROUND-TRIP TEST (this PC). Same as server.localtest.psd1 but pointed at the REAL
    # Mothership so equipped items actually save and load, under a clearly-named stand-in user
    # (mock clients have no Meta account, so there is no mothershipId to key on).
    # LOCAL TEST server (this PC only). For mock-client debugging: teleports, forced triggers,
    # netvar overrides from a local folder. Deliberately does NOT register with, or fetch from, the
    # live Rigel backend -- so it never appears in the server browser and never creates station rows.
    #   .\Start-Server.ps1 -Config server.localtest.psd1
    # ---------------------------------------------------------------------------------
    GameBuild     = '22284'
    GameExe       = '..\..\Nov15\A2\Binaries\Win64\A2-Win64-Shipping.exe'
    Configuration = 'Release'
    DllPath       = ''
    InitWaitSeconds     = 25
    ReadyTimeoutSeconds = 150

    Args = @('-nullrhi', '-nohmd', '-nosound', '-unattended', '-nosplash', '-log')

    # No dashboard / registration: point the backend at a closed loopback port so those calls fail
    # fast and harmlessly instead of reaching production.
    DashboardApiUrl = ''
    DashboardApiKey = ''
    DeploymentId    = 'localtest'
    RegisterIp      = '127.0.0.1'
    LoadGamemode    = ''
    GamemodeSlot    = ''

    ExtraArgs = @(
        '-NoAuthGate'                       # mock clients never do dashboard auth
        '-TeamOverlap'                      # same as the VPS server
        '-QuestTestNoOrg'                   # register mock players' quests without a dashboard org
        '-QuestTestSeed'                    # ...seeded with 3 completed quests (incl. the parkour intro)
        '-ServerName=Rigel-LOCALTEST'
        '-MothershipHost=rigel-ms.wwiggles.org'
        '-MothershipPort=443'
        '-ServerApiKey=5d10900556da4e3935d23a18f9450920000eb79c21b2ad601fe0ecd322bacc12'
        '-CosmeticTestUser=mocktest-cosmetics'
        '-BackendHost=127.0.0.1'
        '-BackendPort=9'
        # netvar overrides are read from this folder (localtest.txt / _latest.txt) instead of the backend
        '-NetvarDir=C:\Users\Zern\AppData\Local\Temp\claude\C--Users-Zern-Documents-OrionDriftStuff\e1127066-f64a-456c-9a3d-24070d7a41d5\scratchpad\nvdir'
    )

    LogDir = 'logs'
}
