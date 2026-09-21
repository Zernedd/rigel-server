@{
    # ---------------------------------------------------------------------------------
    # LOCAL TEST server for the Spec Editor (this PC only).
    #   .\Start-Server.ps1 -Config server.specedit.psd1
    #
    # Same closed-loopback shape as server.questtest.psd1 -- it never registers with, or fetches
    # from, the live Rigel backend, so it cannot appear in the server browser or touch production.
    # The only addition is -SpecEdit, which arms the editor command handler. That handler still
    # refuses everyone unless spec_editors.txt sits next to the game exe and lists their org id, so
    # this profile on its own grants nothing.
    # ---------------------------------------------------------------------------------
    GameBuild     = '22284'
    GameExe       = '..\..\Nov15\A2\Binaries\Win64\A2-Win64-Shipping.exe'
    Configuration = 'Release'
    DllPath       = ''
    # Inject early. When this host's Oculus sign-in cannot produce an entitlement, the engine's
    # "A Shipping build would exit" verdict lands right after engine init (seen 2026-09-21; it used to
    # take ~30s). The payload now flips that exit the moment it loads, so it only has to load before
    # engine init finishes. It waits for the world itself, so early is safe.
    InitWaitSeconds     = 2
    ReadyTimeoutSeconds = 150

    Args = @('-nullrhi', '-nohmd', '-nosound', '-unattended', '-nosplash', '-log')

    DashboardApiUrl = ''
    DashboardApiKey = ''
    DeploymentId    = 'localtest'
    RegisterIp      = '127.0.0.1'
    LoadGamemode    = ''
    GamemodeSlot    = ''

    ExtraArgs = @(
        '-NoAuthGate'
        '-TeamOverlap'
        '-SpecEdit'                         # arm the Spec Editor command handler (allowlist still applies)
        '-SpecEditLocalTest'                # no dashboard here, so no player ever gets an org id -- admit
                                            # org-less callers. LOCAL ONLY: never on a reachable server.
        '-QuestTestNoOrg'                   # without these a no-auth local server never sends players their
        '-QuestTestSeed'                    # quests, so authored quests would have no template and no one to go to
        '-ServerName=Rigel-SPECEDIT'
        '-MothershipHost=127.0.0.1'
        '-MothershipPort=9'                 # discard port: nothing listens
        '-BackendHost=127.0.0.1'
        '-BackendPort=9'
    )

    LogDir = 'logs'
}
