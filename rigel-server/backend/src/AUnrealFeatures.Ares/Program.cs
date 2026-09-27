using AUnrealFeatures.Hosting;
using AUnrealFeatures.Hosting.Application;
using AUnrealFeatures.Hosting.Database;
using AUnrealFeatures.Hosting.Database.Interfaces;
using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.Ares.Servers;
using AUnrealFeatures.AAMothership;
using AUnrealFeatures.AAMothership.Models;
using AUnrealFeatures.EOSSDK;
using AUnrealFeatures.HalcyonSocket;

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
                .AddServer<IEosWsServer, EosWsServer>()

                //   Halcyon socket - :9095 HTTP control (localhost) + :9100 agent TCP. The dashboard's
                //   "Spin Up" relays through it to a connected allocator agent, which launches and
                //   injects a game server that then self-registers. Keep 9095/9100 firewalled from the
                //   internet: the agent protocol has no authentication.
                .AddServer<IHalcyonSocketServer, HalcyonSocketServer>();

            // The watchdog decides a server is down; taking it OUT of the station browser needs the EOS
            // session store and the deployment table, which live over here. Wiring it as a hook keeps
            // HalcyonSocket's single project reference intact (Ares -> HalcyonSocket, never back).
            // A crashed or frozen server never deletes its own session, so without this the station
            // stays listed and players keep joining a corpse for the whole restart.
            ServerWatchdog.TakeDeploymentOffline = (deploymentId, reason) =>
            {
                var dropped = EosGatewayServer.RemoveSessionsByDeployment(deploymentId);

                var depCol = Database.GetCollection<DeploymentDbObject>(true);
                var dep    = depCol?.FindAll().FirstOrDefault(d => d.DeploymentId == deploymentId);
                if (dep != null)
                {
                    dep.Online      = false;
                    dep.PlayerCount = 0;
                    depCol!.Update(dep);
                }
                Console.WriteLine($"[Watchdog] took deployment {deploymentId} out of the browser ({reason}): " +
                                  $"{dropped} session(s) dropped, db row {(dep != null ? "marked offline" : "not found")}");
            };

            // Hand the EOS gateway every station's browser whitelist before serving. It cannot read them
            // itself (Ares -> EOSSDK, never back), and an allowlist with no data fails OPEN -- so without
            // this a backend restart would silently unhide every private station until the next config edit.
            StationAcl.PushAll();
            StationBans.Start();   // and the ban lists (banned accounts never get the banned station's servers)

            await _hostApplication.RunAsync();
        }
    }
}
