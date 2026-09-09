using AUnrealFeatures.Hosting;
using AUnrealFeatures.Hosting.Application;
using AUnrealFeatures.Hosting.Database;
using AUnrealFeatures.Hosting.Database.Interfaces;
using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.Ares.Servers;
using AUnrealFeatures.AAMothership;
using AUnrealFeatures.AAMothership.Models;
using AUnrealFeatures.EOSSDK;

namespace AUnrealFeatures.Ares
{
    /// <summary>
    /// Backend host, trimmed to the Mothership + station/dashboard servers.
    ///
    ///   :90    MothershipServer     player auth and player data (the client's BaseUrl)
    ///   :78    A2StationDbServer    station / deployment API - what the game server DLL
    ///                               registers against (register_server, update_player_count)
    ///   :8080  AresDashboardServer  read API + the dashboard's backend
    ///
    /// The EOS gateway, Playfab, Virtex and the Halcyon socket server were removed. EOSSDK
    /// and HalcyonSocket are still referenced because the two servers above touch a couple
    /// of their static stores (the in-memory EOS session list, the pending-name table);
    /// those keep working with the servers themselves switched off, they simply are not
    /// served over HTTP. See the commented AddServer lines to turn them back on.
    /// </summary>
    public static class Program
    {
        private static IDatabase _database;
        private static IHostApplication _hostApplication;

        public static IDatabase Database => _database;

        public static async Task Main(string[] args)
        {
            ModuleInitialization.Initialize();
            _hostApplication = HostApplication.New("Astra")
                .ConfigureServices(services =>
                {
                    var databaseManager = AstraDatabaseBuilder.Shared
                        .WithDatabase("production", new DatabaseOptions
                        {
                            databaseId = 1,
                            friendlyName = "Production Database",
                            usePassword = true,
                            password = "THU6PW1mxzJqbpmPEyANKcbcRwCaHlWq"
                        })
                        .WithDatabase("development", new DatabaseOptions
                        {
                            databaseId = 2,
                            friendlyName = "Development Database",
                            usePassword = false,
                            password = string.Empty
                        })
                        .Build();

                    if (databaseManager != null)
                    {
                        services.AddSingleton<AstraDatabaseManager, IDatabaseManager>(
                            (AstraDatabaseManager?)databaseManager
                        );

#if DEBUG
                        var database = (AstraDatabase?)databaseManager.GetDatabase(name: "development", id: 2);
#elif !DEBUG
                        var database = (AstraDatabase?)databaseManager.GetDatabase(name: "production", id: 1);
#endif

                        _database = database;
                        services.AddSingleton<AstraDatabase, IDatabase>(database);

                        // ── stations, deployments, events, dashboard accounts ──
                        database?.GetCollection<UserDataResponse>(true);
                        database?.GetCollection<RoleResponse>(true);
                        database?.GetCollection<StationDbObject>(true);
                        database?.GetCollection<DeploymentDbObject>(true);
                        database?.GetCollection<ServerEventDbObject>(true);
                        database?.GetCollection<StationEventDbObject>(true);
                        database?.GetCollection<EventSignupDbObject>(true);
                        database?.GetCollection<UserApiKeyDbObject>(true);

                        // ── Mothership ─────────────────────────────────────────
                        MothershipServer.Database = database!;
                        database?.GetCollection<MothershipPlayerDbObject>(true);
                        database?.GetCollection<MothershipSessionDbObject>(true);
                        database?.GetCollection<MothershipV2PlayerDbObject>(true);
                        database?.GetCollection<MothershipAssociationDbObject>(true);
                        database?.GetCollection<MothershipLinkDbObject>(true);
                        database?.GetCollection<MothershipSharedGroupDbObject>(true);
                        database?.GetCollection<MothershipReportDbObject>(true);
                        database?.GetCollection<MothershipBanDbObject>(true);
                        database?.GetCollection<MothershipMuteDbObject>(true);
                    }
                })
                .AddServer<IA2StationDbServer, A2StationDbServer>()      // :78
                .AddServer<IAresDashboardServer, AresDashboardServer>()  // :8080
                .AddServer<IMothershipServer, MothershipServer>()        // :90

                //   EOS gateway :50 + websocket :80 - our stand-in for the Epic Online Services
                //   API. The in-game station/server list is served by
                //   /matchmaking/v1/{deployment_id}/filter on this gateway, so it has to be
                //   running for players to see any servers at all.
                .AddServer<IEosGatewayServer, EosGatewayServer>()
                .AddServer<IEosWsServer, EosWsServer>();

                // Optional, off by default - uncomment (and re-add the using) to run them:
                //
                //   Halcyon socket :9095 - the control channel the dashboard's "Spin Up"
                //   button uses to launch a game server on a host machine.
                // .AddServer<IHalcyonSocketServer, HalcyonSocketServer>()

            await _hostApplication.RunAsync();
        }
    }
}
