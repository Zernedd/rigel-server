using AUnrealFeatures.Hosting.Database.Interfaces;
using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Binding.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.HalcyonSocket;
using AUnrealFeatures.EOSSDK;
using AUnrealFeatures.EOSSDK.Models;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using FuzzySharp;

namespace AUnrealFeatures.Ares.Servers
{
    public interface IA2StationDbServer { }
    public sealed class A2StationDbServer : AstraHttpServer, IA2StationDbServer
    {
        const string HOSTNAME = "localhost";
        const ushort PORT = 78;

        public A2StationDbServer() : base(HOSTNAME, PORT)
        {
        }

        // ─── Helpers ─────────────────────────────────────────────────────────────

        private static string GenerateApiKey(string ownerId, string keyType = "user")
        {
            var keyId = Guid.NewGuid().ToString();
            var createdAt = DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss.ffffff");
            var payload = new { key_id = keyId, key_type = keyType, owner_id = ownerId, created_at = createdAt };
            var header = Base64UrlEncode(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new { alg = "HS256", typ = "JWT" }));
            var body = Base64UrlEncode(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(payload));
            var signing = $"{header}.{body}";
            using var hmac = new System.Security.Cryptography.HMACSHA256(Encoding.UTF8.GetBytes("astra-api-key-secret"));
            return $"{signing}.{Base64UrlEncode(hmac.ComputeHash(Encoding.UTF8.GetBytes(signing)))}";
        }

        private static string Base64UrlEncode(byte[] data)
            => Convert.ToBase64String(data).Replace("+", "-").Replace("/", "_").TrimEnd('=');

        private static string GenerateId()
            => Guid.NewGuid().ToString("N")[..16];

        private static ServerDeploymentResponse DeploymentToResponse(DeploymentDbObject d)
            => new ServerDeploymentResponse
            {
                DeploymentId = d.DeploymentId,
                StationId = d.StationId,
                DeploymentName = d.DeploymentName,
                Region = d.Region,
                IpAddress = d.IpAddress,
                Version = d.Version,
                CreatedAt = d.CreatedAt,
                Online = d.Online,
                LastEventAt = d.LastEvent ?? DateTime.MinValue,
                PlayerCount = d.PlayerCount,
                Config = d.Config
            };

        private static StationResponse StationToResponse(StationDbObject s,
            IEnumerable<DeploymentDbObject>? deps, bool includeDeployments, bool includeConfig)
            => new StationResponse
            {
                StationId = s.StationId,
                StationName = s.StationName,
                CreatedAt = s.CreatedAt,
                Deployments = includeDeployments
                    ? deps?.Select(DeploymentToResponse).ToList()
                    : null,
                Config = includeConfig ? s.Config : null
            };

        private List<RoleResponse> ExpandRoles(IEnumerable<string> roleIds)
        {
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            return roleIds
                .Select(rid => roleCollection?.FindOne(r => r.RoleId == rid))
                .Where(r => r != null)
                .Select(r => r!)
                .ToList();
        }

        private static readonly string[] AllPermissions =
        {
            "admin",
            "station:create", "station:delete", "station:write", "station:read",
            "station_config:write", "station_config:read",
            "role:read", "role:write",
            "global:fleet:create", "global:fleet:delete", "fleet:read", "fleet:join", "fleet:write",
            "fleet_config:read", "fleet_config:write", "config_netvars:write", "custom_config:write",
            "record_entry:write",
            "server_login", "server_event:read", "server_event:write",
            "user:create", "user:delete", "global:user_data", "user_data:read", "user_login",
            "user_permissions:read", "user_roles:read", "user_roles:write",
            "user_ban:write", "user_ban_short:write", "user_ban:update", "user_ban:revoke",
            "user_mute", "user_kick", "user_warn",
            "global_voip", "dev_inputs",
            "key_holder", "key_holder_circuitlounge", "key_holder_driftplex", "key_holder_complex", "key_holder_office",
            "color:red", "color:yellow", "color:green",
            "anonymous_mode",
            "fleet_report:read", "fleet_report:write", "self:fleet_report:read", "self:fleet_report:write"
        };

        private List<UserPermission> GetPermissionsForUser(UserDataResponse user)
        {
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var perms = new List<UserPermission>();
            foreach (var roleId in user.Roles ?? new List<string>())
            {
                var role = roleCollection?.FindOne(r => r.RoleId == roleId);
                if (role?.Permissions == null) continue;
                if (role.Permissions.Contains("global:admin"))
                {
                    foreach (var perm in AllPermissions)
                        perms.Add(new UserPermission { StationId = "global", Permission = perm });
                    return perms;
                }
                foreach (var perm in role.Permissions)
                    perms.Add(new UserPermission { StationId = role.StationId, Permission = perm });
            }
            return perms;
        }

        // The client (UA2StationDashboardSubsystem) only recognizes these three role
        // names, verbatim. Each station gets its own copy of them.
        private static readonly (string Name, string[] Perms)[] DefaultStationRoles =
        {
            // Full explicit permission set (not the "global:admin" sentinel), so both the
            // login path and the /roles route return real, game-recognized tokens.
            ("Admin",         AllPermissions),
            ("Event Manager", new[]
            {
                "server_event:read", "server_event:write",
                "station:read", "station:write",
                "station_config:read", "station_config:write",
                "config_netvars:write", "custom_config:write",
                "user_kick", "user_mute", "user_warn", "user_roles:read"
            }),
            ("Helper",        new[] { "station:read", "user_kick", "user_mute", "user_warn" }),
        };

        // ─── STATION BOARD CONFIG (district info-boards / signs) ─────────────────────
        // The game's dedicated server, once it has -DashboardDeploymentId/-DashboardApiUrl/
        // -DashboardApiKey, fetches GET /v1/deployments/{id}?include_station_config=true and
        // natively applies the returned config's key/value pairs as NetVars — which is how the
        // BP_Promoboard_* boards (ULiveNetvarImageLoader) get their texture URLs.
        //
        // [2026-09-09] The hardcoded third-party image host (postimg) is GONE. Board images are now
        // UPLOADED and served by this backend itself: POST /v1/board/upload writes the file under
        // BoardUploadDir and stores the resulting self-hosted URL in that station's Config, which
        // already takes precedence over these defaults (see the TryAdd order in the deployment
        // handlers — station.Config is added FIRST, so anything uploaded wins).
        //
        // These defaults therefore no longer carry any URL. A board with no uploaded image simply
        // has no NetVar and stays blank, instead of silently pointing every station at an image on
        // somebody else's server that we do not control and cannot revoke.
        public static readonly string[] BoardConfigKeys =
        {
            "BoardTextureUrl1", "BoardTextureUrl2", "BoardTextureUrl3",
            "BoardTextureUrl4", "BoardTextureUrl5", "BoardTextureUrl6",
            "SignPlazaFront", "SignPlazaWest", "SignPlazaEast",
            "SignComplexWest", "SignComplexEast", "SignComplexBasement",
            "SignArenaPrime",
            "statusBoardTKBMainUrl", "statusBoardTKBSidesUrl",
            "statusBoardTKBBack1Url", "statusBoardTKBBack2Url",
            "kingsCourtTeamLogosUrl",
        };

        // Dashboard-set netvar overrides live in the station Config under this reserved prefix:
        //   nv.module.<SlotID>.<Variable>   a gamemode config variable (UModuleState::GetGamemode*Variable)
        //   nv.world.<path>                  a world netvar, e.g. config/player/brakeStrength
        // They are never served in the game's native config fetch (the station keys there are applied
        // straight into config/stationConfig); the injected DLL polls /v1/deployments/{id}/netvar_overrides
        // and applies them itself.
        public const string NetvarOverridePrefix = "nv.";

        // Where uploaded board images live on disk, and the max size we accept.
        private static readonly string BoardUploadDir =
            Path.Combine(Environment.CurrentDirectory, "boards");
        private const int MaxBoardBytes = 8 * 1024 * 1024;   // 8 MB

        private static readonly Dictionary<string, string> DefaultStationConfig = new()
        {
            ["StationAnnouncement"] = "Welcome to Halcyon Ring",
        };

        // [2026-09-09 REGRESSION FIX] Dropping the board keys from the served config ENTIRELY made
        // clients CRASH ON JOIN: the BP_Promoboard_* loaders (ULiveNetvarImageLoader) expect a NetVar
        // per board and cannot cope with it being absent. The original code's own comment said as much
        // -- "empty on the baked default, so we serve them here" -- i.e. the game relies on the backend
        // to supply these. Removing the third-party URL was right; removing the KEYS was not.
        //
        // So every board key is served again, but pointing at an image WE host (/board/_default.png)
        // rather than someone else's server. An uploaded image still wins, because the deployment
        // handlers TryAdd station.Config BEFORE these defaults.
        public const string DefaultBoardFile = "_default.png";

        // A 64x64 dark-grey PNG, written to the boards folder on first use. Small, neutral, and always
        // present, so a station with nothing uploaded shows a blank plate instead of killing the client.
        private const string DefaultBoardPngBase64 =
            "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAAWklEQVR42u3QMQEAAAjDMOZf9DDB" +
            "RSChSc/aRAEFFFBAAQUUUEABBRRQQAEFFFBAAQUUUEABBRRQQAEFFFBAAQUUUEABBRRQQAEFFFBA" +
            "AQUUUEABBd4WuAAB0wABtAJ4wgAAAABJRU5ErkJggg==";

        internal static void EnsureDefaultBoardImage()
        {
            try
            {
                Directory.CreateDirectory(BoardUploadDir);
                string f = Path.Combine(BoardUploadDir, DefaultBoardFile);
                if (!File.Exists(f))
                    File.WriteAllBytes(f, Convert.FromBase64String(DefaultBoardPngBase64));
            }
            catch (Exception ex) { Console.WriteLine($"[BOARD] could not write default board image: {ex.Message}"); }
        }

        // Fill in any board key the station has not overridden. `host` is the request's own Host, so the
        // URL we hand the game server is one its CLIENTS can actually resolve.
        internal static void AddBoardDefaults(Dictionary<string, string> cfg, string host)
        {
            EnsureDefaultBoardImage();
            string baseUrl = (Environment.GetEnvironmentVariable("BOARD_PUBLIC_BASE") ?? "").TrimEnd('/');
            if (string.IsNullOrWhiteSpace(baseUrl))
                baseUrl = PublicBoardBase(host);
            string url = $"{baseUrl}/board/{DefaultBoardFile}";
            foreach (var k in BoardConfigKeys)
                cfg.TryAdd(k, url);
        }

        // The game server fetches this config over LOOPBACK (127.0.0.1:78), so request.Host is
        // "127.0.0.1" -- a URL the HEADSETS cannot resolve. Never hand back a loopback or private
        // address as a board URL; fall back to the known public origin instead. BOARD_PUBLIC_BASE
        // overrides this entirely.
        internal static string PublicBoardBase(string host)
        {
            const string fallback = "https://rigel.wwiggles.org";
            if (string.IsNullOrWhiteSpace(host)) return fallback;
            string h = host.Split(':')[0];
            if (h.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
                h.StartsWith("127.") || h.StartsWith("10.") || h.StartsWith("192.168.") ||
                h == "::1" || h.StartsWith("172.16.") || h.StartsWith("169.254."))
                return fallback;
            return $"https://{host}";
        }

        // The server authenticates with this fixed key (launch arg -DashboardApiKey=). Lets
        // log_in_with_key succeed without a per-user key so the deployment/config fetch runs.
        private const string ServerMasterKey = "halcyon-server-key";

        // A2 client build this backend serves. register_server stamps this onto every server's DB row +
        // EOS session (Bucket/BuildId); the client's server search matches on build, so this MUST equal
        // the LIVE fleet's build or every server gets filtered out of the browser.
        // ⚠ PRODUCTION IS 20996. Do NOT bump this to 29932 until the 29932 build is what's actually live —
        // bumping it while 20996 servers are hosting stamps them 29932 and hides them from 20996 clients
        // (the 2026-08-25 outage). The 29932 dev work uses the v2 routes + HTTP list (no hard version
        // filter), so it does not need this constant bumped. Proper long-term fix: add a `version` field to
        // RegisterServerRequest so the DLL reports its own build and both can coexist.
        // Rigel serves the 22284 (Nov15) client, so the fleet build IS 22284. A server registered
        // (and the browser filter) must be stamped 22284 or the 22284 client filters every station out.
        private const string BuildVersion = "22284";
        private const int    BuildIdNum   = 22284;

        // Meta app creds for UserProof nonce validation on /users/log_in. Quest and Rift are separate
        // apps; a nonce is only valid under the app that minted it, and this endpoint serves BOTH, so
        // we try each set and accept if either validates (deny only if neither). Keep in sync with the
        // Mothership META_APP_*/RIFT_APP_* constants.
        // Two switches so the new code adds ZERO cost to the live login path until opted in:
        //   NonceVerify=false -> don't even call graph (login path identical to before). Flip true to
        //                        start the cached/fail-open verify + [A2DB][LOGIN] logging (capture).
        //   NonceEnforce=true -> actually DENY when the verified nonce is invalid (needs NonceVerify).
        private const bool   NonceVerify     = false;
        private const bool   NonceEnforce    = false;
        private const string QUEST_APP_ID    = "2236834297114138";
        private const string QUEST_APP_SECRET= "099561c7c859625e28a2e27998649aad";
        private const string RIFT_APP_ID     = "2240289680101933";
        private const string RIFT_APP_SECRET = "a23fa419167728390ff2a0cebbcdae3b";
        // 4s timeout (not 8s): AstraHttpServer's accept loop processes requests SERIALLY, so any time
        // spent in here stalls EVERY other request behind it. Keep the worst case short.
        private static readonly System.Net.Http.HttpClient NonceHttp =
            new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(4) };
        // Anti-jam cache: userId -> unix-seconds expiry. A user who validated once doesn't re-hit graph
        // on every login (launch-day: each unique user causes ~1 outbound call, not one per login). Only
        // a genuine success is cached, so a pirate who never validates is never cached in.
        private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, long> _nonceOkCache = new();
        private const long NonceCacheTtlSec = 12 * 3600;

        // Single-app nonce check. Returns: 1 = valid, 0 = definitively invalid, -1 = graph error/timeout
        // (no answer). The tri-state lets the caller FAIL-OPEN on an outage instead of locking players out.
        private static async Task<int> VerifyNonceWith(string accessToken, string userId, string nonce)
        {
            try
            {
                var form = new System.Net.Http.FormUrlEncodedContent(new Dictionary<string, string>
                {
                    ["access_token"] = accessToken,
                    ["nonce"]        = nonce,
                    ["user_id"]      = userId,
                });
                var resp = await NonceHttp.PostAsync("https://graph.oculus.com/user_nonce_validate", form);
                var text = await resp.Content.ReadAsStringAsync();
                using var doc = JsonDocument.Parse(text);
                bool ok = doc.RootElement.TryGetProperty("is_valid", out var v) &&
                          (v.ValueKind == JsonValueKind.True ||
                           (v.ValueKind == JsonValueKind.String && v.GetString()?.ToLowerInvariant() == "true"));
                return ok ? 1 : 0;
            }
            catch { return -1; }   // unreachable / timeout / parse failure -> no definitive answer
        }

        // Try Quest creds first, then Rift; valid on EITHER. Cache-first (no I/O on a repeat login).
        // FAIL-OPEN: allow if NO app said valid but at least one call errored (a Meta blip must not
        // lock out the whole player base on launch); DENY only when both apps returned a definitive
        // "not valid". A legit player logs in with one app's nonce; a forged/absent nonce is rejected.
        private static async Task<bool> VerifyNonceAny(string userId, string nonce)
        {
            if (string.IsNullOrEmpty(userId) || string.IsNullOrEmpty(nonce)) return false;

            var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
            if (_nonceOkCache.TryGetValue(userId, out var exp) && exp > now) return true;   // cache hit
            if (exp != 0 && exp <= now) _nonceOkCache.TryRemove(userId, out _);              // evict stale

            // Quest first. A definitive pass caches + allows; a graph error means Meta is unreachable, so
            // FAIL-OPEN immediately (don't burn a second timeout on Rift when graph is clearly down).
            var q = await VerifyNonceWith($"OC|{QUEST_APP_ID}|{QUEST_APP_SECRET}", userId, nonce);
            if (q == 1) { _nonceOkCache[userId] = now + NonceCacheTtlSec; return true; }
            if (q == -1) { Console.WriteLine($"[A2DB][LOGIN] graph unreachable (quest) '{userId}' -> FAIL-OPEN allow"); return true; }

            // Quest said definitively "not valid" (graph is up) -> could be a Rift user; check Rift.
            var r = await VerifyNonceWith($"OC|{RIFT_APP_ID}|{RIFT_APP_SECRET}", userId, nonce);
            if (r == 1) { _nonceOkCache[userId] = now + NonceCacheTtlSec; return true; }
            if (r == -1) { Console.WriteLine($"[A2DB][LOGIN] graph unreachable (rift) '{userId}' -> FAIL-OPEN allow"); return true; }
            return false;      // both apps definitively not-valid -> forged/absent nonce
        }

        // Idempotently ensures Admin / Event Manager / Helper exist for a station.
        private void EnsureStationRoles(string stationId)
        {
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            if (roleCollection == null) return;

            foreach (var (name, perms) in DefaultStationRoles)
            {
                if (roleCollection.FindOne(r => r.StationId == stationId && r.RoleName == name) != null)
                    continue;

                roleCollection.Insert(new RoleResponse
                {
                    RoleId = GenerateId(),
                    StationId = stationId,
                    RoleName = name,
                    RoleDescription = "",
                    Permissions = perms.ToList()
                });
            }
        }

        // ─── DEDICATED SERVER REGISTRATION ───────────────────────────────────────

        // POST /register_server — a dedicated server self-registers on spin-up.
        // Creates a unique station + online deployment + roles, and registers the
        // EOS matchmaking session so the game's server search finds it. Returns the
        // station_id the server should then use (for the /roles path, etc.).
        [HttpPost("/register_server")]
        public async Task<IHttpActionResult> RegisterServer(IHttpRequest request, IHttpResponse response)
        {
            RegisterServerRequest? body;
            try { body = JsonSerializer.Deserialize<RegisterServerRequest>(request.Body); }
            catch { body = null; }
            if (body == null)
                return Results.Ok(new RegisterServerResponse { Success = false });

            var ip           = string.IsNullOrWhiteSpace(body.Ip)         ? "127.0.0.1"                 : body.Ip;
            var port         = string.IsNullOrWhiteSpace(body.Port)       ? "7777"                      : body.Port;
            var maxPlayers   = body.MaxPlayers > 0 ? body.MaxPlayers : 10;

            // If the caller supplied a deployment id (allocator decided it pre-launch and passed it to
            // the game as -DashboardDeploymentId), register under THAT id and reuse its station on a
            // re-register (upsert) so we don't spawn a new station per boot. Otherwise mint fresh.
            var depCol = Program.Database.GetCollection<DeploymentDbObject>(true);
            var existingDep = !string.IsNullOrWhiteSpace(body.DeploymentId)
                ? depCol?.FindAll().FirstOrDefault(d => d.DeploymentId == body.DeploymentId)
                : null;

            var deploymentId = !string.IsNullOrWhiteSpace(body.DeploymentId)
                ? body.DeploymentId!
                : Guid.NewGuid().ToString("N");
            var stationId    = existingDep?.StationId ?? GenerateId();
            var serverName   = string.IsNullOrWhiteSpace(body.ServerName) ? $"Server_{stationId[..6]}"  : body.ServerName;

            // A name typed in the dashboard "Spin Up" modal wins over the DLL's default. HalcyonSocket
            // stashed it against this deployment id at spin-up time. It stays sticky across re-registers
            // via the existing deployment row's name once the pending entry is consumed.
            if (HalcyonSocketServer.PendingNames.TryRemove(deploymentId, out var dashName) && !string.IsNullOrWhiteSpace(dashName))
                serverName = dashName;
            else if (existingDep != null && !string.IsNullOrWhiteSpace(existingDep.DeploymentName))
                serverName = existingDep.DeploymentName!;

            var stationCol0 = Program.Database.GetCollection<StationDbObject>(true);
            if (stationCol0?.FindAll().FirstOrDefault(s => s.StationId == stationId) == null)
                stationCol0?.Insert(new StationDbObject
                {
                    StationId   = stationId,
                    StationName = serverName,
                    CreatedAt   = DateTime.Now,
                    Online      = true,
                    LastOnline  = DateTime.Now,
                    Config      = new Dictionary<string, string>()
                });

            var depRow = new DeploymentDbObject
            {
                DeploymentId   = deploymentId,
                StationId      = stationId,
                DeploymentName = serverName,
                IpAddress      = $"{ip}:{port}",
                Region         = string.IsNullOrWhiteSpace(body.Region) ? "NAE" : body.Region,
                Version        = BuildVersion,
                CreatedAt      = existingDep?.CreatedAt ?? DateTime.Now,
                Online         = true,
                PlayerCount    = 0,
                Config         = new Dictionary<string, string>()
            };
            if (existingDep != null) depCol?.Update(depRow);
            else                     depCol?.Insert(depRow);

            EnsureStationRoles(stationId);

            EosGatewayServer.UpsertSession(new EosSessionInfo
            {
                Id            = Guid.NewGuid().ToString(),
                ServerName    = serverName,
                IpAddress     = ip,
                Port          = port,
                MaxPlayers    = maxPlayers,
                Bucket        = BuildVersion,
                BuildId       = BuildIdNum,
                ImguiPort     = body.ImguiPort ?? "",
                StationId     = stationId,
                DeploymentId  = deploymentId,
                PublicPlayers = new List<string>()
            });

            Logger.Warning($"Dedicated server registered | station={stationId} deployment={deploymentId} {ip}:{port}");
            return Results.Ok(new RegisterServerResponse
            {
                Success      = true,
                StationId    = stationId,
                DeploymentId = deploymentId
            });
        }

        // POST /update_player_count — periodic heartbeat from a dedicated server reporting its REAL
        // connected-player count (netdriver ClientConnections). The displayed count comes from the EOS
        // session's PublicPlayers, which only clients mutate, so hard disconnects (no leave DELETE)
        // leave ghosts and the count stays stuck high. Reconcile the session down to the reported count
        // and mirror it onto the DB deployment row.
        [HttpPost("/update_player_count")]
        public async Task<IHttpActionResult> UpdatePlayerCount(IHttpRequest request, IHttpResponse response)
        {
            UpdatePlayerCountRequest? body;
            try { body = JsonSerializer.Deserialize<UpdatePlayerCountRequest>(request.Body); }
            catch { body = null; }
            if (body == null || string.IsNullOrWhiteSpace(body.DeploymentId))
                return Results.Ok(new { success = false, error = "deployment_id required" });

            var count   = body.PlayerCount < 0 ? 0 : body.PlayerCount;
            var matched = EosGatewayServer.SetSessionPlayerCount(body.DeploymentId!, count);

            var depCol = Program.Database.GetCollection<DeploymentDbObject>(true);
            var dep    = depCol?.FindAll().FirstOrDefault(d => d.DeploymentId == body.DeploymentId);
            if (dep != null)
            {
                dep.PlayerCount = count;
                dep.LastEvent   = DateTime.Now;
                depCol!.Update(dep);
            }

            return Results.Ok(new { success = true, deployment_id = body.DeploymentId, player_count = count, session_matched = matched });
        }

        // POST /server_heartbeat — the watchdog's liveness feed from a dedicated server, every 5s.
        // Unauthenticated on purpose, exactly like /register_server and /update_player_count: it is the
        // game server talking to its own backend, and the worst a forged post can do is make the
        // watchdog think a dead server is healthy (it can never cause a restart, which needs the agent
        // to confirm the process state on the box). See ServerWatchdog for what is done with it.
        [HttpPost("/server_heartbeat")]
        public async Task<IHttpActionResult> ServerHeartbeat(IHttpRequest request, IHttpResponse response)
        {
            ServerHeartbeatRequest? body;
            try { body = JsonSerializer.Deserialize<ServerHeartbeatRequest>(request.Body); }
            catch { body = null; }
            if (body == null || string.IsNullOrWhiteSpace(body.DeploymentId))
                return Results.Ok(new { success = false, error = "deployment_id required" });

            HalcyonSocketServer.Instance?.Watchdog.OnHeartbeat(
                body.DeploymentId!, body.Pid, body.Seq, body.DispatchAgeMs, body.TickAgeMs,
                body.UptimeMs, body.Players);

            // Keep the deployment row's freshness stamp moving too, so anything reading LastEvent sees
            // a live server between the 3-minute player-count reports.
            var depCol = Program.Database.GetCollection<DeploymentDbObject>(true);
            var dep    = depCol?.FindAll().FirstOrDefault(d => d.DeploymentId == body.DeploymentId);
            if (dep != null) { dep.LastEvent = DateTime.Now; depCol!.Update(dep); }

            return Results.Ok(new { success = true });
        }

        // POST /v1/server/admin/purge_stations — dev cleanup: wipe ALL stations + deployments +
        // roles (they accumulated one-per-boot before deployment ids were made stable). Gated by
        // the server master key (x-api-key). Destructive; returns the counts removed.
        [HttpPost("/v1/server/admin/purge_stations")]
        public Task<IHttpActionResult> PurgeStations(IHttpRequest request, IHttpResponse response)
        {
            if (request.GetHeaderValue("x-api-key") != ServerMasterKey)
                return Task.FromResult<IHttpActionResult>(Results.Ok(new SuccessBoolean { Success = false }));

            // DeleteAll() rather than DeleteMany(_ => true) — LiteDB's expression visitor NREs on a
            // constant-true predicate. Return a plain typed ack (anonymous types tripped the serializer).
            int st = Program.Database.GetCollection<StationDbObject>(true)?.DeleteAll()    ?? 0;
            int dp = Program.Database.GetCollection<DeploymentDbObject>(true)?.DeleteAll() ?? 0;
            int rl = Program.Database.GetCollection<RoleResponse>(true)?.DeleteAll()       ?? 0;
            // The DB rows above aren't what the GAME lists — that's driven by the in-memory EOS
            // matchmaking sessions. Clear those too or purged stations keep showing in-game.
            int es = EosGatewayServer.ClearSessions();
            Logger.Warning($"PURGE stations={st} deployments={dp} roles={rl} eos_sessions={es}");
            return Task.FromResult<IHttpActionResult>(Results.Ok(new SuccessBoolean { Success = true }));
        }

        // ─── USERS ───────────────────────────────────────────────────────────────

        // GET /users (deprecated v0) — returns raw array of users filtered by last_login
        [HttpGet("/users")]
        public async Task<IHttpActionResult> ListAllUsersV0(IHttpRequest request, IHttpResponse response)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            DateTime lastLoginTimeEpsilon = request.Queries.ContainsKey("last_login")
                ? DateTime.Parse(request.GetQueryParameter("last_login", ""))
                : DateTime.MinValue;

            return Results.Ok(userCollection?.FindAll()
                .Where(x => x.LastLogin >= lastLoginTimeEpsilon)
                .ToArray() ?? Array.Empty<UserDataResponse>());
        }

        // POST /users — create or update user
        [HttpPost("/users")]
        public async Task<IHttpActionResult> CreateOrUpdateUser(
            IHttpRequest request,
            IHttpResponse response,
            [FromBody] UserRequest serverRequest)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            if (userCollection == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            Logger.Warning(JsonSerializer.Serialize(serverRequest));
            var selectedUser = userCollection.FindOne(x => x.UserId == serverRequest.UserId);
            if (selectedUser == null)
            {
                userCollection.Insert(new UserDataResponse
                {
                    UserId = serverRequest.UserId,
                    Username = "User",
                    DiscordId = serverRequest.DiscordId,
                    Platform = serverRequest.Platform,
                    CreatedAt = DateTime.Now,
                    LastLogin = DateTime.MinValue,
                    Roles = new List<string>(),
                    Bans = new List<BanRequest>()
                });
                return Results.Ok(new SuccessBoolean { Success = true });
            }

            selectedUser.Username = serverRequest.Username;
            selectedUser.DiscordId = serverRequest.DiscordId;
            selectedUser.Platform = serverRequest.Platform;
            userCollection.Update(selectedUser);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // GET /v1/users — paginated, fuzzy search
        [HttpGet("v1/users")]
        public async Task<IHttpActionResult> ListAllUsersV1(IHttpRequest request, IHttpResponse response)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "100") ?? "100");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");
            string searchString = request.GetQueryParameter("search_string", "") ?? "";
            string lastLogin = request.GetQueryParameter("last_login", "") ?? "";

            var users = userCollection?.FindAll().ToList() ?? new List<UserDataResponse>();

            if (!string.IsNullOrEmpty(lastLogin))
            {
                var loginThreshold = DateTime.Parse(lastLogin);
                users = users.Where(u => u.LastLogin >= loginThreshold).ToList();
            }

            List<UserDataResponse> filtered;
            if (!string.IsNullOrEmpty(searchString))
            {
                var matched = Process.ExtractTop(searchString, users.Select(x => x.Username))
                    .Select(x => x.Value)
                    .ToHashSet();
                filtered = users.Where(u => matched.Contains(u.Username)).ToList();
            }
            else
            {
                filtered = users;
            }

            return Results.Ok(
                PagedResponse<UserDataResponse>.Create<UsersResponsePage>(filtered, pageSize, page));
        }

        // DELETE /users/{user_id}
        [HttpDelete("/users/{user_id}")]
        public async Task<IHttpActionResult> DeleteUser(
            IHttpRequest request, IHttpResponse response, string user_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            userCollection!.Delete(user.Id);

            // remove api keys
            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            foreach (var key in keyCollection?.Find(k => k.UserId == user_id).ToList() ?? new())
                keyCollection!.Delete(key.Id);

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // GET /users/{user_id} (deprecated v0) — returns list of StationPerms
        [HttpGet("/users/{user_id}")]
        public async Task<IHttpActionResult> GetUserV0(
            IHttpRequest request, IHttpResponse response, string user_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(Array.Empty<StationPerm>());

            return Results.Ok(GetPermissionsForUser(user).ToArray());
        }

        // GET /v1/users/{user_id} — full user data with optional role/permission expansion
        [HttpGet("v1/users/{user_id}")]
        public async Task<IHttpActionResult> GetUserV1(
            IHttpRequest request, IHttpResponse response, string user_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok((object?)null);

            bool includeRoles = bool.Parse(request.GetQueryParameter("include_roles", "false") ?? "false");
            bool includePerms = bool.Parse(request.GetQueryParameter("include_permissions", "false") ?? "false");
            bool includeBans = bool.Parse(request.GetQueryParameter("include_bans", "false") ?? "false");

            List<RoleResponse>? roles = null;
            if (includeRoles || includePerms)
                roles = ExpandRoles(user.Roles ?? new List<string>());

            return Results.Ok(UserDataV1Response.FromStorage(user, roles, includeBans));
        }

        // POST /users/log_in_server — server-side login (requires root key)
        [HttpPost("/users/log_in_server")]
        public async Task<IHttpActionResult> UserServerLogin(IHttpRequest request, IHttpResponse response)
        {
            LogInServerRequest? body;
            try { body = JsonSerializer.Deserialize<LogInServerRequest>(request.Body); }
            catch { body = null; }

            if (body == null || string.IsNullOrEmpty(body.UserId))
                return Results.Ok(new LogInServerResponse { ApiKey = "", User = null!, Permissions = new() });

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == body.UserId);
            if (user == null)
            {
                user = new UserDataResponse
                {
                    UserId = body.UserId,
                    Username = body.Username,
                    Platform = body.Platform,
                    CreatedAt = DateTime.Now,
                    LastLogin = DateTime.Now,
                    Roles = new List<string>(),
                    Bans = new List<BanRequest>()
                };
                userCollection?.Insert(user);
            }
            else
            {
                user.Username = body.Username;
                user.Platform = body.Platform;
                user.LastLogin = DateTime.Now;
                userCollection?.Update(user);
            }

            var apiKey = GenerateApiKey(user.UserId, "server");
            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            keyCollection?.Insert(new UserApiKeyDbObject { UserId = user.UserId, ApiKey = apiKey });

            return Results.Ok(new LogInServerResponse
            {
                ApiKey = apiKey,
                User = user,
                Permissions = GetPermissionsForUser(user)
            });
        }

        // POST /users/log_in_with_key — login using existing api key from header
        [HttpPost("/users/log_in_with_key")]
        public async Task<IHttpActionResult> LogInWithKey(IHttpRequest request, IHttpResponse response)
        {
            var apiKey = request.GetHeaderValue("x-api-key", "");
            if (string.IsNullOrEmpty(apiKey))
                return Results.Ok((object?)null);

            // The dedicated server authenticates with the fixed launch-arg key (-DashboardApiKey=).
            // Accept it directly (no per-user key needed) so the deployment/station-config fetch runs.
            if (apiKey == ServerMasterKey)
                return Results.Ok(new LogInServerResponse
                {
                    ApiKey = apiKey,
                    User = new UserDataResponse
                    {
                        UserId = "halcyon-server", Username = "HalcyonServer",
                        Roles = new List<string>(), Bans = new List<BanRequest>()
                    },
                    Permissions = AllPermissions
                        .Select(p => new UserPermission { StationId = "global", Permission = p })
                        .ToList()
                });

            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            var keyEntry = keyCollection?.FindOne(k => k.ApiKey == apiKey);
            if (keyEntry == null)
                return Results.Ok((object?)null);

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == keyEntry.UserId);
            if (user == null)
                return Results.Ok((object?)null);

            return Results.Ok(new LogInServerResponse
            {
                ApiKey = apiKey,
                User = user,
                Permissions = GetPermissionsForUser(user)
            });
        }

        // POST /users/log_in — player login with nonce
        [HttpPost("/users/log_in")]
        public async Task<IHttpActionResult> UserPlayerLogin(IHttpRequest request, IHttpResponse response)
        {
            ClientLoginRequest? body;
            try { body = JsonSerializer.Deserialize<ClientLoginRequest>(request.Body); }
            catch { body = null; }

            if (body == null)
                return Results.Ok(new ClientLoginResponse
                {
                    Success = false, ApiKey = "", OrgScopedId = "", ServerDeployments = new()
                });

            // Both Quest and Rift players hit this endpoint with a UserProof nonce. When NonceVerify is
            // on, verify it owns the claimed platform id (against both app cred sets) — a client that
            // never authenticated with Meta can't produce a valid nonce. Skipped entirely when off so
            // the live login path is untouched (no graph call).
            if (NonceVerify)
            {
                var nonceOk = await VerifyNonceAny(body.PlatformId, body.Nonce);
                Console.WriteLine($"[A2DB][LOGIN] platformId='{body.PlatformId}' nonceLen={(body.Nonce ?? "").Length} nonceOk={nonceOk} enforce={NonceEnforce}");
                if (NonceEnforce && !nonceOk)
                    return Results.Ok(new ClientLoginResponse
                    {
                        Success = false, ApiKey = "", OrgScopedId = "", ServerDeployments = new()
                    });
            }

            var userId = body.PlatformId;
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == userId);

            if (user == null)
            {
                user = new UserDataResponse
                {
                    UserId = userId,
                    Username = body.Username,
                    Platform = body.Platform,
                    CreatedAt = DateTime.Now,
                    LastLogin = DateTime.Now,
                    Roles = new List<string>(),
                    Bans = new List<BanRequest>()
                };
                userCollection?.Insert(user);
            }
            else
            {
                user.Username = body.Username;
                user.Platform = body.Platform;
                user.LastLogin = DateTime.Now;
                userCollection?.Update(user);
            }

            var apiKey = GenerateApiKey(userId);
            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            keyCollection?.Insert(new UserApiKeyDbObject { UserId = userId, ApiKey = apiKey });

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var onlineDeployments = deploymentCollection?.FindAll()
                .Where(d => d.Online)
                .Select(d =>
                {
                    var station = stationCollection?.FindOne(s => s.StationId == d.StationId);
                    return new ClientLoginDeployment
                    {
                        DeploymentId = d.DeploymentId,
                        StationId = d.StationId,
                        StationName = station?.StationName ?? d.StationId,
                        DeploymentName = d.DeploymentName,
                        IpAddress = d.IpAddress,
                        IsWhitelist = false,
                        IsPublic = true,
                        IsJoinable = d.Online
                    };
                }).ToList() ?? new List<ClientLoginDeployment>();

            // merge in EOS matchmaking sessions as joinable deployments
            var eosIds = new HashSet<string>(onlineDeployments.Select(d => d.DeploymentId));
            foreach (var s in GetEosSessions())
            {
                if (eosIds.Contains(s.Id)) continue;
                var station = stationCollection?.FindOne(st => st.StationId == (s.StationId ?? s.Bucket));
                onlineDeployments.Add(new ClientLoginDeployment
                {
                    DeploymentId   = s.Id,
                    StationId      = s.StationId ?? s.Bucket,
                    StationName    = station?.StationName ?? s.ServerName,
                    DeploymentName = s.ServerName,
                    IpAddress      = string.IsNullOrEmpty(s.Port) ? s.IpAddress : $"{s.IpAddress}:{s.Port}",
                    IsWhitelist    = false,
                    IsPublic       = true,
                    IsJoinable     = true
                });
            }

            return Results.Ok(new ClientLoginResponse
            {
                Success = true,
                ApiKey = apiKey,
                OrgScopedId = userId,
                ServerDeployments = onlineDeployments
            });
        }

        // POST /users/{user_id}/api_key — generate a new api key for user
        [HttpPost("/users/{user_id}/api_key")]
        public async Task<IHttpActionResult> CreateUserApiKey(
            IHttpRequest request, IHttpResponse response, string user_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new ApiKeyResponse { ApiKey = "" });

            var apiKey = GenerateApiKey(user_id);
            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            keyCollection?.Insert(new UserApiKeyDbObject { UserId = user_id, ApiKey = apiKey });

            return Results.Ok(new ApiKeyResponse { ApiKey = apiKey });
        }

        // ─── STATIONS ─────────────────────────────────────────────────────────────

        // POST /stations/create
        [HttpPost("/stations/create")]
        public async Task<IHttpActionResult> CreateStation(IHttpRequest request, IHttpResponse response)
        {
            CreateStationRequest? body;
            try { body = JsonSerializer.Deserialize<CreateStationRequest>(request.Body); }
            catch { body = null; }

            if (body == null || string.IsNullOrEmpty(body.StationName))
                return Results.Ok(new CreateStationResponse { StationId = "" });

            var stationId = string.IsNullOrEmpty(body.StationId) ? GenerateId() : body.StationId;
            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);

            var existing = stationCollection?.FindOne(s => s.StationId == stationId);
            if (existing != null)
                return Results.Ok(new CreateStationResponse { StationId = stationId });

            stationCollection?.Insert(new StationDbObject
            {
                StationId = stationId,
                StationName = body.StationName,
                CreatedAt = DateTime.Now,
                Online = false,
                Config = new Dictionary<string, string>()
            });

            EnsureStationRoles(stationId);

            return Results.Ok(new CreateStationResponse { StationId = stationId });
        }

        // GET /stations (deprecated v0) — returns array of StationResponse
        [HttpGet("/stations")]
        public async Task<IHttpActionResult> FetchAllStationsV0(IHttpRequest request, IHttpResponse response)
        {
            bool includeConfig = bool.Parse(request.GetQueryParameter("include_config", "false") ?? "false");
            bool includeDeployments = bool.Parse(request.GetQueryParameter("include_deployments", "false") ?? "false");
            bool includeOffline = bool.Parse(request.GetQueryParameter("include_offline_stations", "true") ?? "true");

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);

            var stations = stationCollection?.FindAll().ToList() ?? new List<StationDbObject>();
            if (!includeOffline)
                stations = stations.Where(s => s.Online).ToList();

            var result = stations.Select(s =>
            {
                IEnumerable<DeploymentDbObject>? deps = includeDeployments
                    ? deploymentCollection?.Find(d => d.StationId == s.StationId)
                    : null;
                return StationToResponse(s, deps, includeDeployments, includeConfig);
            }).ToArray();

            return Results.Ok(result);
        }

        // GET /v1/stations — paginated stations
        [HttpGet("v1/stations")]
        public async Task<IHttpActionResult> FetchAllStationsV1(IHttpRequest request, IHttpResponse response)
        {
            bool includeConfig = bool.Parse(request.GetQueryParameter("include_config", "false") ?? "false");
            bool includeDeployments = bool.Parse(request.GetQueryParameter("include_deployments", "false") ?? "false");
            bool includeOffline = bool.Parse(request.GetQueryParameter("include_offline_stations", "true") ?? "true");
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "100") ?? "100");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);

            var stations = stationCollection?.FindAll().ToList() ?? new List<StationDbObject>();
            if (!includeOffline)
                stations = stations.Where(s => s.Online).ToList();

            var responses = stations.Select(s =>
            {
                IEnumerable<DeploymentDbObject>? deps = includeDeployments
                    ? deploymentCollection?.Find(d => d.StationId == s.StationId)
                    : null;
                return StationToResponse(s, deps, includeDeployments, includeConfig);
            }).ToList();

            return Results.Ok(PagedResponse<StationResponse>.Create<StationResponsePage>(responses, pageSize, page));
        }

        // GET /stations/{station_id} — also handles /stations/player_count
        [HttpGet("/stations/{station_id}")]
        public async Task<IHttpActionResult> GetStation(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            // handle player_count route in case it lands here
            if (station_id == "player_count")
            {
                var depsCol = Program.Database.GetCollection<DeploymentDbObject>(true);
                int total = depsCol?.FindAll().Where(d => d.Online).Sum(d => d.PlayerCount) ?? 0;
                return Results.Ok(total);
            }

            bool includeConfig = bool.Parse(request.GetQueryParameter("include_config", "true") ?? "true");
            bool includeDeployments = bool.Parse(request.GetQueryParameter("include_deployments", "true") ?? "true");

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            if (station == null)
                return Results.Ok((object?)null);

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            IEnumerable<DeploymentDbObject>? deps = includeDeployments
                ? deploymentCollection?.Find(d => d.StationId == station_id)
                : null;

            return Results.Ok(StationToResponse(station, deps, includeDeployments, includeConfig));
        }

        // GET /stations/player_count — sum player counts of online deployments
        [HttpGet("/stations/player_count")]
        public async Task<IHttpActionResult> GetPlayerCount(IHttpRequest request, IHttpResponse response)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            int total = deploymentCollection?.FindAll().Where(d => d.Online).Sum(d => d.PlayerCount) ?? 0;
            return Results.Ok(total);
        }

        // PATCH /stations/{station_id} — update station name
        [HttpPatch("/stations/{station_id}")]
        public async Task<IHttpActionResult> UpdateStation(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            UpdateStationRequest? body;
            try { body = JsonSerializer.Deserialize<UpdateStationRequest>(request.Body); }
            catch { body = null; }

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            if (station == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            if (body != null && !string.IsNullOrEmpty(body.StationName))
                station.StationName = body.StationName;

            stationCollection?.Update(station);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // DELETE /stations/{station_id}
        [HttpDelete("/stations/{station_id}")]
        public async Task<IHttpActionResult> DeleteStation(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            if (station == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            stationCollection!.Delete(station.Id);

            // delete all associated deployments
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            foreach (var dep in deploymentCollection?.Find(d => d.StationId == station_id).ToList() ?? new())
                deploymentCollection!.Delete(dep.Id);

            // delete all roles for station
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            foreach (var role in roleCollection?.Find(r => r.StationId == station_id).ToList() ?? new())
                roleCollection!.Delete(role.Id);

            // delete all events for station
            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            foreach (var ev in eventCollection?.Find(e => e.StationId == station_id).ToList() ?? new())
                eventCollection!.Delete(ev.Id);

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // --- BOARD IMAGE UPLOAD ------------------------------------------------------
        // Replaces the old hardcoded third-party image URL. Upload once, and the image is served
        // by this backend from the same public origin the client already talks to, so there is no
        // external host to expire, rate-limit, or go down.
        //
        //   POST /v1/board/upload?station_id=<id>&key=<BoardTextureUrl1|SignPlazaFront|...>
        //   header:  x-api-key: <server master key>
        //   body:    the raw image bytes (png/jpg/gif/webp)
        //
        // Writes boards/<station>_<key>_<n>.<ext>, points that station's Config[key] at
        // /board/<file>, and returns the URL. Station config already overrides the defaults (the
        // deployment handlers TryAdd station.Config FIRST), so the board picks it up on the game
        // server's next deployment-config fetch.
        //
        // The public base URL matters: the Quest client fetches these directly, so it must be the
        // externally reachable origin, not 127.0.0.1. Taken from BOARD_PUBLIC_BASE when set, else
        // derived from the request's own Host header (correct whenever the upload came in through
        // the same public hostname the clients use).
        [HttpPost("/v1/board/upload")]
        public Task<IHttpActionResult> UploadBoardImage(IHttpRequest request, IHttpResponse response)
        {
            if (request.GetHeaderValue("x-api-key") != ServerMasterKey)
                return Task.FromResult<IHttpActionResult>(Results.Unauthorized(new SuccessBoolean { Success = false }));

            string stationId = request.GetQueryParameter("station_id", "") ?? "";
            string key       = request.GetQueryParameter("key", "") ?? "";
            if (string.IsNullOrWhiteSpace(stationId) || string.IsNullOrWhiteSpace(key))
                return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "station_id and key are required" }));
            if (Array.IndexOf(BoardConfigKeys, key) < 0)
                return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "unknown board key", allowed = BoardConfigKeys }));

            byte[] body = request.Body ?? Array.Empty<byte>();
            if (body.Length == 0)
                return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "empty body" }));
            if (body.Length > MaxBoardBytes)
                return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "image too large", maxBytes = MaxBoardBytes }));

            // Sniff the real format from magic bytes rather than trusting a caller-supplied
            // extension -- that is what decides the filename we later serve it back under.
            string ext = SniffImageExtension(body);
            if (ext == null)
                return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "not a png/jpg/gif/webp image" }));

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == stationId);
            if (station == null)
                return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id = stationId }));

            Directory.CreateDirectory(BoardUploadDir);
            // Cache-busting suffix: boards are fetched by URL, so reusing a filename would leave
            // clients showing the previous image out of their own HTTP cache.
            string safeStation = SanitiseForFileName(stationId);
            string fileName = $"{safeStation}_{key}_{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}.{ext}";
            string fullPath = Path.Combine(BoardUploadDir, fileName);

            // Drop this station+key's previous uploads so the folder cannot grow without bound.
            try
            {
                foreach (var stale in Directory.GetFiles(BoardUploadDir, $"{safeStation}_{key}_*"))
                    File.Delete(stale);
            }
            catch (Exception ex) { Logger.Warning($"board upload: could not clear old files: {ex.Message}"); }

            File.WriteAllBytes(fullPath, body);

            string baseUrl = (Environment.GetEnvironmentVariable("BOARD_PUBLIC_BASE") ?? "").TrimEnd('/');
            if (string.IsNullOrWhiteSpace(baseUrl))
                baseUrl = PublicBoardBase(request.Host);   // never store a loopback URL
            string url = $"{baseUrl}/board/{fileName}";

            station.Config[key] = url;
            stationCollection?.Update(station);

            Logger.Information($"board upload: station={stationId} key={key} bytes={body.Length} -> {url}");
            return Task.FromResult<IHttpActionResult>(Results.Ok(new { success = true, key, url, bytes = body.Length }));
        }

        // GET /board/{file} -- serve an uploaded board image.
        [HttpGet("/board/{file}")]
        public Task<IHttpActionResult> GetBoardImage(IHttpRequest request, IHttpResponse response, string file)
        {
            // Path-traversal guard: only ever serve a bare filename out of the upload directory.
            if (string.IsNullOrWhiteSpace(file) || file.Contains("..") ||
                file.Contains('/') || file.Contains('\\') || Path.IsPathRooted(file))
                return Task.FromResult<IHttpActionResult>(Results.NotFound());

            string fullPath = Path.Combine(BoardUploadDir, file);
            if (!File.Exists(fullPath))
                return Task.FromResult<IHttpActionResult>(Results.NotFound());

            // Results.Ok() REPLACES whatever was written onto `response`, which served an empty
            // text/plain body. Results.Configurable(status, mime, bytes) is how this framework
            // returns a raw file -- it is what HttpStaticFilesProcessor uses.
            string mime = Path.GetExtension(file).ToLowerInvariant() switch
            {
                ".png"  => "image/png",
                ".jpg"  => "image/jpeg",
                ".gif"  => "image/gif",
                ".webp" => "image/webp",
                _       => "application/octet-stream",
            };
            return Task.FromResult<IHttpActionResult>(
                Results.Configurable(HttpStatusCode.OK, mime, File.ReadAllBytes(fullPath)));
        }

        // GET /v1/board/keys -- the uploadable board slots, for the dashboard UI to render.
        [HttpGet("/v1/board/keys")]
        public Task<IHttpActionResult> GetBoardKeys(IHttpRequest request, IHttpResponse response)
            => Task.FromResult<IHttpActionResult>(Results.Ok(BoardConfigKeys));

        // Identify the image type from its magic bytes. Returns null if it is not one we accept.
        private static string SniffImageExtension(byte[] b)
        {
            if (b.Length >= 8  && b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) return "png";
            if (b.Length >= 3  && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)                 return "jpg";
            if (b.Length >= 6  && b[0] == 0x47 && b[1] == 0x49 && b[2] == 0x46)                 return "gif";
            if (b.Length >= 12 && b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46
                               && b[8] == 0x57 && b[9] == 0x45 && b[10] == 0x42 && b[11] == 0x50) return "webp";
            return null;
        }

        // Shared with DashboardServer, which handles the browser-side upload on :8080 while this
        // server keeps GET /board/{file} for public serving. Same process, same folder, same DB.
        public static string BoardUploadDirPublic => BoardUploadDir;
        public static string SniffImageExtensionPublic(byte[] b) => SniffImageExtension(b);
        public static string SanitiseForFileNamePublic(string s) => SanitiseForFileName(s);

        private static string SanitiseForFileName(string s)
        {
            var sb = new StringBuilder(s.Length);
            foreach (char c in s)
                sb.Append(char.IsLetterOrDigit(c) ? c : '-');
            return sb.ToString();
        }

        // GET /stations/{station_id}/config
        [HttpGet("/stations/{station_id}/config")]
        public async Task<IHttpActionResult> GetStationConfig(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            return Results.Ok(station?.Config ?? new Dictionary<string, string>());
        }

        // POST /stations/{station_id}/config — merge new keys into config
        [HttpPost("/stations/{station_id}/config")]
        public async Task<IHttpActionResult> SetStationConfig(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            Dictionary<string, string>? data;
            try { data = JsonSerializer.Deserialize<Dictionary<string, string>>(request.Body); }
            catch { data = null; }

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            if (station == null)
                return Results.Ok(new Dictionary<string, string>());

            if (data != null)
                foreach (var kv in data)
                    station.Config[kv.Key] = kv.Value;

            stationCollection?.Update(station);
            return Results.Ok(station.Config);
        }

        // DELETE /stations/{station_id}/config — remove keys from config
        [HttpDelete("/stations/{station_id}/config")]
        public async Task<IHttpActionResult> DeleteStationConfig(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            List<string>? keys;
            try { keys = JsonSerializer.Deserialize<List<string>>(request.Body); }
            catch { keys = null; }

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var station = stationCollection?.FindOne(s => s.StationId == station_id);
            if (station == null)
                return Results.Ok(new Dictionary<string, string>());

            if (keys != null)
                foreach (var k in keys)
                    station.Config.Remove(k);

            stationCollection?.Update(station);
            return Results.Ok(station.Config);
        }

        // GET /stations/{station_id}/online
        [HttpGet("/stations/{station_id}/online")]
        public async Task<IHttpActionResult> GetStationOnline(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            bool isOnline = deploymentCollection?.FindAll()
                .Any(d => d.StationId == station_id && d.Online) ?? false;
            return Results.Ok(isOnline);
        }

        // GET /stations/{station_id}/users (deprecated v0)
        [HttpGet("/stations/{station_id}/users")]
        public async Task<IHttpActionResult> GetAllUsersInStationV0(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);

            var stationRoleIds = roleCollection?.Find(r => r.StationId == station_id)
                .Select(r => r.RoleId).ToHashSet() ?? new HashSet<string>();

            var usersInStation = userCollection?.FindAll()
                .Where(u => (u.Roles ?? new List<string>()).Any(rid => stationRoleIds.Contains(rid)))
                .ToArray() ?? Array.Empty<UserDataResponse>();

            return Results.Ok(usersInStation);
        }

        // GET /v1/stations/{station_id}/users
        [HttpGet("v1/stations/{station_id}/users")]
        public async Task<IHttpActionResult> GetAllUsersInStationV1(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "100") ?? "100");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");
            string searchString = request.GetQueryParameter("search_string", "") ?? "";

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);

            var stationRoleIds = roleCollection?.Find(r => r.StationId == station_id)
                .Select(r => r.RoleId).ToHashSet() ?? new HashSet<string>();

            var users = userCollection?.FindAll()
                .Where(u => (u.Roles ?? new List<string>()).Any(rid => stationRoleIds.Contains(rid)))
                .ToList() ?? new List<UserDataResponse>();

            if (!string.IsNullOrEmpty(searchString))
            {
                var matched = Process.ExtractTop(searchString, users.Select(x => x.Username))
                    .Select(x => x.Value).ToHashSet();
                users = users.Where(u => matched.Contains(u.Username)).ToList();
            }

            return Results.Ok(PagedResponse<UserDataResponse>.Create<UsersResponsePage>(users, pageSize, page));
        }

        // Users who are Admin on EVERY station (present and future) regardless of any
        // explicit grant. Add account IDs here for permanent global admin.
        private static readonly HashSet<string> GlobalAdmins = new() { "8721004894622429", "28147570424869138", "28379411968414630", "27847584954897406", "27931067463212467" };

        // GET /stations/{station_id}/users/{user_id}/roles
        [HttpGet("/stations/{station_id}/users/{user_id}/roles")]
        public async Task<IHttpActionResult> GetStationUserRoles(
            IHttpRequest request, IHttpResponse response, string station_id, string user_id)
        {
            EnsureStationRoles(station_id);

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);

            // Global admins get the station's Admin role automatically on any station.
            if (GlobalAdmins.Contains(user_id))
            {
                var adminRole = roleCollection?.FindAll()
                    .FirstOrDefault(r => r.StationId == station_id && r.RoleName == "Admin");
                return Results.Ok(new RolesResponse
                {
                    Roles = adminRole != null ? new List<RoleResponse> { adminRole } : new List<RoleResponse>()
                });
            }

            // FindAll().FirstOrDefault: captured-variable FindOne hangs this DB.
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindAll().FirstOrDefault(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new RolesResponse { Roles = new List<RoleResponse>() });

            var roleList = roleCollection?.FindAll().ToList() ?? new List<RoleResponse>();
            var roles = (user.Roles ?? new List<string>())
                .Select(rid => roleList.FirstOrDefault(r => r.RoleId == rid && r.StationId == station_id))
                .Where(r => r != null)
                .Select(r => r!)
                .ToList();

            return Results.Ok(new RolesResponse { Roles = roles });
        }

        // POST /stations/{station_id}/users/{user_id}/roles/{role_id}
        [HttpPost("/stations/{station_id}/users/{user_id}/roles/{role_id}")]
        public async Task<IHttpActionResult> AddStationUserRole(
            IHttpRequest request, IHttpResponse response,
            string station_id, string user_id, string role_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            user.Roles ??= new List<string>();
            if (!user.Roles.Contains(role_id))
            {
                user.Roles.Add(role_id);
                userCollection?.Update(user);
            }
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // POST /stations/{station_id}/users/{user_id}/grant/{role_name}
        // Convenience: assign a role by NAME (Admin / Event Manager / Helper) — no id lookup.
        [HttpPost("/stations/{station_id}/users/{user_id}/grant/{role_name}")]
        public async Task<IHttpActionResult> GrantStationUserRoleByName(
            IHttpRequest request, IHttpResponse response,
            string station_id, string user_id, string role_name)
        {
            EnsureStationRoles(station_id);

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            // FindOne with captured-variable predicates hangs on this DB's query
            // translator; FindAll().FirstOrDefault evaluates in-memory and is safe.
            var role = roleCollection?.FindAll().FirstOrDefault(r => r.StationId == station_id && r.RoleName == role_name);
            if (role == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindAll().FirstOrDefault(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            user.Roles ??= new List<string>();
            if (!user.Roles.Contains(role.RoleId))
            {
                user.Roles.Add(role.RoleId);
                userCollection?.Update(user);
            }
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // DELETE /stations/{station_id}/users/{user_id}/role/{role_id}
        [HttpDelete("/stations/{station_id}/users/{user_id}/role/{role_id}")]
        public async Task<IHttpActionResult> DeleteStationUserRole(
            IHttpRequest request, IHttpResponse response,
            string station_id, string user_id, string role_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            bool removed = user.Roles?.Remove(role_id) ?? false;
            if (removed)
                userCollection?.Update(user);

            return Results.Ok(new SuccessBoolean { Success = removed });
        }

        // POST /v1/stations/{station_id}/user_roles — add role by username
        [HttpPost("v1/stations/{station_id}/user_roles")]
        public async Task<IHttpActionResult> AddStationUserRoleByName(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            UserRoleByNameRequest? body;
            try { body = JsonSerializer.Deserialize<UserRoleByNameRequest>(request.Body); }
            catch { body = null; }

            if (body == null)
                return Results.Ok(new UserRoleByNameResponse { Success = false, UserExists = false });

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.Username == body.Username);
            if (user == null)
                return Results.Ok(new UserRoleByNameResponse { Success = false, UserExists = false });

            user.Roles ??= new List<string>();
            if (!user.Roles.Contains(body.RoleId))
            {
                user.Roles.Add(body.RoleId);
                userCollection?.Update(user);
            }
            return Results.Ok(new UserRoleByNameResponse { Success = true, UserExists = true });
        }

        // GET /stations/{station_id}/roles
        [HttpGet("/stations/{station_id}/roles")]
        public async Task<IHttpActionResult> GetStationRoles(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            EnsureStationRoles(station_id);

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var roles = roleCollection?.Find(r => r.StationId == station_id).ToList()
                ?? new List<RoleResponse>();
            return Results.Ok(new RolesResponse { Roles = roles });
        }

        // POST /stations/{station_id}/roles — create role
        [HttpPost("/stations/{station_id}/roles")]
        public async Task<IHttpActionResult> CreateStationRole(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            CreateRoleRequest? body;
            try { body = JsonSerializer.Deserialize<CreateRoleRequest>(request.Body); }
            catch { body = null; }

            if (body == null || string.IsNullOrWhiteSpace(body.RoleName))
                return Results.Ok((object?)null);

            var roleId = GenerateId();
            var role = new RoleResponse
            {
                RoleId = roleId,
                StationId = station_id,
                RoleName = body.RoleName,
                RoleDescription = body.RoleDescription ?? "",
                Permissions = new List<string>()
            };

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            roleCollection?.Insert(role);

            return Results.Ok(new RoleData
            {
                RoleId = roleId,
                StationId = station_id,
                RoleName = role.RoleName,
                RoleDescription = role.RoleDescription
            });
        }

        // DELETE /stations/{station_id}/roles/{role_id}
        [HttpDelete("/stations/{station_id}/roles/{role_id}")]
        public async Task<IHttpActionResult> DeleteStationRole(
            IHttpRequest request, IHttpResponse response, string station_id, string role_id)
        {
            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var role = roleCollection?.FindOne(r => r.RoleId == role_id && r.StationId == station_id);
            if (role == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            roleCollection!.Delete(role.Id);

            // remove role from all users who have it
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            foreach (var user in userCollection?.FindAll().Where(u => u.Roles != null && u.Roles.Contains(role_id)).ToList() ?? new())
            {
                user.Roles!.Remove(role_id);
                userCollection!.Update(user);
            }

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // PATCH /stations/{station_id}/roles/{role_id} — update role name/description
        [HttpPatch("/stations/{station_id}/roles/{role_id}")]
        public async Task<IHttpActionResult> UpdateStationRole(
            IHttpRequest request, IHttpResponse response, string station_id, string role_id)
        {
            CreateRoleRequest? body;
            try { body = JsonSerializer.Deserialize<CreateRoleRequest>(request.Body); }
            catch { body = null; }

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var role = roleCollection?.FindOne(r => r.RoleId == role_id && r.StationId == station_id);
            if (role == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            if (body != null)
            {
                if (!string.IsNullOrWhiteSpace(body.RoleName)) role.RoleName = body.RoleName;
                if (body.RoleDescription != null) role.RoleDescription = body.RoleDescription;
            }
            roleCollection?.Update(role);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // PATCH /stations/{station_id}/roles/{role_id}/permissions
        [HttpPatch("/stations/{station_id}/roles/{role_id}/permissions")]
        public async Task<IHttpActionResult> AssignPerms(
            IHttpRequest request, IHttpResponse response, string station_id, string role_id)
        {
            AssignPermsBody? body;
            try { body = JsonSerializer.Deserialize<AssignPermsBody>(request.Body); }
            catch { body = null; }

            var roleCollection = Program.Database.GetCollection<RoleResponse>(true);
            var role = roleCollection?.FindOne(r => r.RoleId == role_id && r.StationId == station_id);
            if (role == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            role.Permissions = body?.Permissions ?? role.Permissions;
            roleCollection?.Update(role);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // GET /v1/stations/{station_id}/roles/{role_id}/users
        [HttpGet("v1/stations/{station_id}/roles/{role_id}/users")]
        public async Task<IHttpActionResult> GetRoleUsers(
            IHttpRequest request, IHttpResponse response, string station_id, string role_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var users = userCollection?.FindAll()
                .Where(u => u.Roles != null && u.Roles.Contains(role_id))
                .ToList() ?? new List<UserDataResponse>();

            return Results.Ok(new UsersResponse { Users = users });
        }

        // GET /stations/{station_id}/server_events (deprecated v0)
        [HttpGet("/stations/{station_id}/server_events")]
        public async Task<IHttpActionResult> GetServerEventsV0(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "500") ?? "500");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var eventCollection = Program.Database.GetCollection<ServerEventDbObject>(true);

            var depIds = deploymentCollection?.Find(d => d.StationId == station_id)
                .Select(d => d.DeploymentId).ToHashSet() ?? new HashSet<string>();

            var events = eventCollection?.FindAll()
                .Where(e => depIds.Contains(e.DeploymentId))
                .OrderByDescending(e => e.Timestamp)
                .Take(pageSize)
                .Select((e, i) => new ServerEventResponse
                {
                    Index = i + 1,
                    EventType = e.EventType,
                    DeploymentId = e.DeploymentId,
                    EventData = e.EventData,
                    Timestamp = e.Timestamp
                }).ToArray() ?? Array.Empty<ServerEventResponse>();

            return Results.Ok(events);
        }

        // GET /v1/stations/{station_id}/server_events
        [HttpGet("v1/stations/{station_id}/server_events")]
        public async Task<IHttpActionResult> GetServerEventsV1(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "500") ?? "500");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");
            string? eventType = request.GetQueryParameter("event_type");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var eventCollection = Program.Database.GetCollection<ServerEventDbObject>(true);

            var depIds = deploymentCollection?.Find(d => d.StationId == station_id)
                .Select(d => d.DeploymentId).ToHashSet() ?? new HashSet<string>();

            var query = eventCollection?.FindAll()
                .Where(e => depIds.Contains(e.DeploymentId)) ?? Enumerable.Empty<ServerEventDbObject>();

            if (!string.IsNullOrEmpty(eventType))
                query = query.Where(e => e.EventType == eventType);

            var allEvents = query.OrderByDescending(e => e.Timestamp).ToList();
            var mapped = allEvents.Select((e, i) => new ServerEventResponse
            {
                Index = i + 1,
                EventType = e.EventType,
                DeploymentId = e.DeploymentId,
                EventData = e.EventData,
                Timestamp = e.Timestamp
            }).ToList();

            return Results.Ok(PagedResponse<ServerEventResponse>.Create<ServerEventResponsePage>(mapped, pageSize, page));
        }

        // POST /v1/stations/{station_id}/users/{user_id}/ban
        [HttpPost("v1/stations/{station_id}/users/{user_id}/ban")]
        public async Task<IHttpActionResult> BanStationUser(
            IHttpRequest request, IHttpResponse response, string station_id, string user_id)
        {
            int duration = int.Parse(request.GetQueryParameter("duration", "0") ?? "0");

            BanRequest? body;
            try { body = JsonSerializer.Deserialize<BanRequest>(request.Body); }
            catch { body = null; }

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            user.Bans ??= new List<BanRequest>();
            user.Bans.Add(new BanRequest
            {
                UserId = user_id,
                StationId = station_id,
                Expiration = duration > 0 ? DateTime.Now.AddSeconds(duration) : DateTime.MaxValue,
                Reason = body?.Reason ?? "",
                Revoked = false
            });
            userCollection?.Update(user);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // PATCH /v1/stations/{station_id}/users/{user_id}/unban
        [HttpPatch("v1/stations/{station_id}/users/{user_id}/unban")]
        public async Task<IHttpActionResult> UnbanStationUser(
            IHttpRequest request, IHttpResponse response, string station_id, string user_id)
        {
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == user_id);
            if (user == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            bool updated = false;
            foreach (var ban in user.Bans ?? new List<BanRequest>())
            {
                if (ban.StationId == station_id && !ban.Revoked)
                {
                    ban.Revoked = true;
                    updated = true;
                }
            }
            if (updated) userCollection?.Update(user);
            return Results.Ok(new SuccessBoolean { Success = updated });
        }

        // GET /v1/stations/{station_id}/bans
        [HttpGet("v1/stations/{station_id}/bans")]
        public async Task<IHttpActionResult> GetStationBans(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            bool includeRevoked = bool.Parse(request.GetQueryParameter("include_revoked", "false") ?? "false");
            bool includeExpired = bool.Parse(request.GetQueryParameter("include_expired", "false") ?? "false");

            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var now = DateTime.Now;

            var allBans = userCollection?.FindAll()
                .SelectMany(u => (u.Bans ?? new List<BanRequest>())
                    .Where(b => b.StationId == station_id)
                    .Where(b => includeRevoked || !b.Revoked)
                    .Where(b => includeExpired || b.Expiration > now))
                .ToList() ?? new List<BanRequest>();

            return Results.Ok(new BanResponse { Bans = allBans });
        }

        // POST /v1/stations/{station_id}/event — create station event
        [HttpPost("v1/stations/{station_id}/event")]
        public async Task<IHttpActionResult> CreateStationEvent(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            string title = request.GetQueryParameter("title", "") ?? "";
            string description = request.GetQueryParameter("description", "") ?? "";
            int duration = int.Parse(request.GetQueryParameter("duration", "0") ?? "0");
            string? deploymentId = request.GetQueryParameter("deployment_id");
            string startTimeStr = request.GetQueryParameter("start_time", DateTime.Now.ToString("o")) ?? DateTime.Now.ToString("o");
            bool isPublic = bool.Parse(request.GetQueryParameter("public", "true") ?? "true");
            bool signupsOpen = bool.Parse(request.GetQueryParameter("signups_open", "true") ?? "true");

            Dictionary<string, string>? config;
            try { config = JsonSerializer.Deserialize<Dictionary<string, string>>(request.Body); }
            catch { config = new Dictionary<string, string>(); }

            var eventId = GenerateId();
            var startTime = DateTime.Parse(startTimeStr);
            var ev = new StationEventDbObject
            {
                EventId = eventId,
                StationId = station_id,
                DeploymentId = string.IsNullOrEmpty(deploymentId) ? null : deploymentId,
                Title = title,
                Description = description,
                StartTime = startTime,
                Duration = duration,
                Public = isPublic,
                SignupsOpen = signupsOpen,
                Config = config ?? new Dictionary<string, string>()
            };

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            eventCollection?.Insert(ev);

            return Results.Ok(new StationEvent
            {
                EventId = ev.EventId,
                StationId = ev.StationId,
                DeploymentId = ev.DeploymentId ?? "",
                Title = ev.Title,
                Description = ev.Description,
                StartTime = ev.StartTime,
                Duration = ev.Duration,
                Public = ev.Public,
                SignupsOpen = ev.SignupsOpen
            });
        }

        // GET /v1/stations/{station_id}/events
        [HttpGet("v1/stations/{station_id}/events")]
        public async Task<IHttpActionResult> GetStationEvents(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            bool getPast = bool.Parse(request.GetQueryParameter("get_past_events", "false") ?? "false");
            var now = DateTime.Now;

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var events = eventCollection?.Find(e => e.StationId == station_id).ToList()
                ?? new List<StationEventDbObject>();

            if (!getPast)
                events = events.Where(e => e.StartTime.AddSeconds(e.Duration) >= now).ToList();

            return Results.Ok(new StationEvents
            {
                Events = events.Select(e => new StationEvent
                {
                    EventId = e.EventId,
                    StationId = e.StationId,
                    DeploymentId = e.DeploymentId ?? "",
                    Title = e.Title,
                    Description = e.Description,
                    StartTime = e.StartTime,
                    Duration = e.Duration,
                    Public = e.Public,
                    SignupsOpen = e.SignupsOpen
                }).ToList()
            });
        }

        // GET /v1/events/{event_id}
        [HttpGet("v1/events/{event_id}")]
        public async Task<IHttpActionResult> GetStationEvent(
            IHttpRequest request, IHttpResponse response, string event_id)
        {
            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id);
            if (ev == null)
                return Results.Ok((object?)null);

            return Results.Ok(new StationEvent
            {
                EventId = ev.EventId,
                StationId = ev.StationId,
                DeploymentId = ev.DeploymentId ?? "",
                Title = ev.Title,
                Description = ev.Description,
                StartTime = ev.StartTime,
                Duration = ev.Duration,
                Public = ev.Public,
                SignupsOpen = ev.SignupsOpen
            });
        }

        // DELETE /v1/stations/{station_id}/event/{event_id}
        [HttpDelete("v1/stations/{station_id}/event/{event_id}")]
        public async Task<IHttpActionResult> DeleteStationEvent(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
            if (ev == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            eventCollection!.Delete(ev.Id);

            // delete signups
            var signupCollection = Program.Database.GetCollection<EventSignupDbObject>(true);
            foreach (var s in signupCollection?.Find(s => s.EventId == event_id).ToList() ?? new())
                signupCollection!.Delete(s.Id);

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // PATCH /v1/stations/{station_id}/event/{event_id}
        [HttpPatch("v1/stations/{station_id}/event/{event_id}")]
        public async Task<IHttpActionResult> UpdateStationEvent(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            StationEvent? body;
            try { body = JsonSerializer.Deserialize<StationEvent>(request.Body); }
            catch { body = null; }

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
            if (ev == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            if (body != null)
            {
                if (!string.IsNullOrEmpty(body.Title)) ev.Title = body.Title;
                if (!string.IsNullOrEmpty(body.Description)) ev.Description = body.Description;
                if (body.StartTime != default) ev.StartTime = body.StartTime;
                if (body.Duration > 0) ev.Duration = body.Duration;
                ev.Public = body.Public;
                ev.SignupsOpen = body.SignupsOpen;
                if (!string.IsNullOrEmpty(body.DeploymentId)) ev.DeploymentId = body.DeploymentId;
            }
            eventCollection?.Update(ev);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // POST /v1/stations/{station_id}/event/{event_id}/users/{user_id}/signup
        [HttpPost("v1/stations/{station_id}/event/{event_id}/users/{user_id}/signup")]
        public async Task<IHttpActionResult> SignupStationEvent(
            IHttpRequest request, IHttpResponse response,
            string station_id, string event_id, string user_id)
        {
            string? data = null;
            try
            {
                if (request.Body.Length > 0)
                    data = JsonSerializer.Deserialize<string>(request.Body);
            }
            catch { data = null; }

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id);
            if (ev == null || !ev.SignupsOpen)
                return Results.Ok(new EventSignupResponse { UserSignedUp = false, SignupId = "" });

            var signupId = GenerateId();
            var signupCollection = Program.Database.GetCollection<EventSignupDbObject>(true);

            // check if already signed up
            var existing = signupCollection?.FindOne(s => s.EventId == event_id && s.UserId == user_id);
            if (existing != null)
                return Results.Ok(new EventSignupResponse { UserSignedUp = true, SignupId = existing.SignupId });

            signupCollection?.Insert(new EventSignupDbObject
            {
                SignupId = signupId,
                EventId = event_id,
                UserId = user_id,
                Timestamp = DateTime.Now,
                Data = data
            });

            return Results.Ok(new EventSignupResponse { UserSignedUp = true, SignupId = signupId });
        }

        // GET /v1/stations/{station_id}/event/{event_id}/signups
        [HttpGet("v1/stations/{station_id}/event/{event_id}/signups")]
        public async Task<IHttpActionResult> GetStationEventSignups(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            var signupCollection = Program.Database.GetCollection<EventSignupDbObject>(true);
            var signups = signupCollection?.Find(s => s.EventId == event_id)
                .Select(s => new StationEventSignupInfo
                {
                    SignupId = s.SignupId,
                    UserId = s.UserId,
                    Timestamp = s.Timestamp,
                    Data = s.Data
                }).ToList() ?? new List<StationEventSignupInfo>();

            return Results.Ok(new StationEventSignups { Signups = signups });
        }

        // DELETE /v1/stations/{station_id}/event/{event_id}/signup
        [HttpDelete("v1/stations/{station_id}/event/{event_id}/signup")]
        public async Task<IHttpActionResult> DeleteStationEventSignup(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            string signupId = request.GetQueryParameter("signup_id", "") ?? "";
            var signupCollection = Program.Database.GetCollection<EventSignupDbObject>(true);
            var signup = signupCollection?.FindOne(s => s.SignupId == signupId && s.EventId == event_id);
            if (signup == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            signupCollection!.Delete(signup.Id);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // GET /v1/stations/{station_id}/event/{event_id}/config
        [HttpGet("v1/stations/{station_id}/event/{event_id}/config")]
        public async Task<IHttpActionResult> GetStationEventConfig(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
            return Results.Ok(ev?.Config ?? new Dictionary<string, string>());
        }

        // POST /v1/stations/{station_id}/event/{event_id}/config — merge keys
        [HttpPost("v1/stations/{station_id}/event/{event_id}/config")]
        public async Task<IHttpActionResult> SetStationEventConfig(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            Dictionary<string, string>? data;
            try { data = JsonSerializer.Deserialize<Dictionary<string, string>>(request.Body); }
            catch { data = null; }

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
            if (ev == null)
                return Results.Ok(new Dictionary<string, string>());

            if (data != null)
                foreach (var kv in data)
                    ev.Config[kv.Key] = kv.Value;

            eventCollection?.Update(ev);
            return Results.Ok(ev.Config);
        }

        // DELETE /v1/stations/{station_id}/event/{event_id}/config — remove keys
        [HttpDelete("v1/stations/{station_id}/event/{event_id}/config")]
        public async Task<IHttpActionResult> DeleteStationEventConfig(
            IHttpRequest request, IHttpResponse response, string station_id, string event_id)
        {
            List<string>? keys;
            try { keys = JsonSerializer.Deserialize<List<string>>(request.Body); }
            catch { keys = null; }

            var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
            var ev = eventCollection?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
            if (ev == null)
                return Results.Ok(new Dictionary<string, string>());

            if (keys != null)
                foreach (var k in keys)
                    ev.Config.Remove(k);

            eventCollection?.Update(ev);
            return Results.Ok(ev.Config);
        }

        // ─── FLEETS ───────────────────────────────────────────────────────────────

        private static Dictionary<string, object>? ToTypedConfig(Dictionary<string, string>? config)
        {
            if (config == null || config.Count == 0) return null;
            var result = new Dictionary<string, object>();
            foreach (var kv in config)
            {
                if (bool.TryParse(kv.Value, out var b))
                    result[kv.Key] = b;
                else if (double.TryParse(kv.Value, System.Globalization.NumberStyles.Any,
                    System.Globalization.CultureInfo.InvariantCulture, out var d))
                    result[kv.Key] = d;
                else
                    result[kv.Key] = kv.Value;
            }
            return result;
        }

        private static List<EosSessionInfo> GetEosSessions()
        {
            lock (EosGatewayServer.Sessions) { return EosGatewayServer.Sessions.ToList(); }
        }

        private static DeploymentDbObject EosSessionToDeployment(EosSessionInfo s)
            => new DeploymentDbObject
            {
                DeploymentId   = s.Id,
                StationId      = s.StationId ?? s.Bucket,
                DeploymentName = s.ServerName,
                IpAddress      = string.IsNullOrEmpty(s.Port) ? s.IpAddress : $"{s.IpAddress}:{s.Port}",
                Version        = s.BuildId.ToString(),
                CreatedAt      = DateTime.UtcNow,
                Online         = true,
                PlayerCount    = s.PublicPlayers.Count,
                Config         = new Dictionary<string, string>
                {
                    ["port"]        = s.Port,
                    ["imgui_port"]  = s.ImguiPort,
                    ["bucket"]      = s.Bucket,
                    ["max_players"] = s.MaxPlayers.ToString()
                }
            };

        private static FleetStationResponse DeploymentToFleetStation(DeploymentDbObject d)
            => new FleetStationResponse
            {
                StationId   = d.DeploymentId,
                FleetId     = d.StationId,
                SessionId   = d.DeploymentId,
                StationName = d.DeploymentName,
                Region      = d.Region,
                Ip          = d.IpAddress,
                Version     = d.Version,
                DeploymentCl = d.Version,
                Created     = d.CreatedAt,
                Online      = d.Online,
                LastEvent   = d.LastEvent,
                PlayerCount = d.PlayerCount,
                Disabled    = false,
                Config      = d.Config?.Count > 0 ? ToTypedConfig(d.Config) : null,
                DistrictPopulations = null
            };

        private static FleetResponse StationToFleet(StationDbObject s, IEnumerable<DeploymentDbObject>? allDeps, bool includeStations, bool includeConfig)
        {
            var depsList = allDeps?.ToList();
            return new FleetResponse
            {
                FleetId   = s.StationId,
                FleetName = s.StationName,
                Created   = s.CreatedAt,
                Online    = depsList?.Any(d => d.Online) ?? s.Online,
                Stations  = includeStations ? depsList?.Select(DeploymentToFleetStation).ToList() : null,
                Config    = includeConfig && s.Config?.Count > 0 ? ToTypedConfig(s.Config) : null
            };
        }

        // GET /v2/fleets — paginated fleets
        [HttpGet("v2/fleets")]
        public async Task<IHttpActionResult> FetchAllFleetsV2(IHttpRequest request, IHttpResponse response)
        {
            bool includeConfig   = bool.Parse(request.GetQueryParameter("include_config",   "false") ?? "false");
            bool includeStations = bool.Parse(request.GetQueryParameter("include_stations", "false") ?? "false");
            bool includeOffline  = bool.Parse(request.GetQueryParameter("include_offline_fleets", "true") ?? "true");
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "100") ?? "100");
            int page     = int.Parse(request.GetQueryParameter("page", "1") ?? "1");

            var stationCollection    = Program.Database.GetCollection<StationDbObject>(true);
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);

            var fleets = stationCollection?.FindAll().ToList() ?? new List<StationDbObject>();
            if (!includeOffline)
                fleets = fleets.Where(s => s.Online).ToList();

            var eosByStation = GetEosSessions()
                .ToLookup(s => s.StationId ?? s.Bucket, s => EosSessionToDeployment(s));

            var responses = fleets.Select(s =>
            {
                IEnumerable<DeploymentDbObject> deps = includeStations
                    ? (deploymentCollection?.Find(d => d.StationId == s.StationId) ?? Enumerable.Empty<DeploymentDbObject>())
                        .Concat(eosByStation[s.StationId])
                    : Enumerable.Empty<DeploymentDbObject>();
                return StationToFleet(s, deps, includeStations, includeConfig);
            }).ToList();

            return Results.Ok(PagedResponse<FleetResponse>.Create<FleetResponsePage>(responses, pageSize, page));
        }

        // GET /v2/fleets/{fleet_id} — single fleet
        [HttpGet("v2/fleets/{fleet_id}")]
        public async Task<IHttpActionResult> GetFleetV2(IHttpRequest request, IHttpResponse response, string fleet_id)
        {
            bool includeConfig   = bool.Parse(request.GetQueryParameter("include_config",   "true") ?? "true");
            bool includeStations = bool.Parse(request.GetQueryParameter("include_stations", "true") ?? "true");

            var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
            var fleet = stationCollection?.FindOne(s => s.StationId == fleet_id);
            if (fleet == null)
                return Results.Ok((object?)null);

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            IEnumerable<DeploymentDbObject> deps = includeStations
                ? (deploymentCollection?.Find(d => d.StationId == fleet_id) ?? Enumerable.Empty<DeploymentDbObject>())
                    .Concat(GetEosSessions()
                        .Where(s => (s.StationId ?? s.Bucket) == fleet_id)
                        .Select(EosSessionToDeployment))
                : Enumerable.Empty<DeploymentDbObject>();

            return Results.Ok(StationToFleet(fleet, deps, includeStations, includeConfig));
        }

        // ─── V2 CLIENT API (A2 build 29932) ──────────────────────────────────────
        // The 29932 client talks a v2 contract (host api.oriondrift.net, overridable via
        // -DashboardApiUrl). These map our existing station/deployment DB onto the new shapes.
        // Model note: our StationDbObject == v2 "fleet", our DeploymentDbObject == v2 "station".

        // GET /v2/stations?include_offline=&version=&page_size=&page= — the browse list. Returns every
        // deployment (DB + live EOS session) as an FStationResponse item. We stamp BuildVersion on each
        // row (see DeploymentToFleetStation via register), so we do NOT hard-filter on the version param
        // (avoids ever hiding our own server); include_offline is honored.
        [HttpGet("v2/stations")]
        public async Task<IHttpActionResult> FetchAllStationsV2(IHttpRequest request, IHttpResponse response)
        {
            bool includeOffline = bool.Parse(request.GetQueryParameter("include_offline", "true") ?? "true");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deps = (deploymentCollection?.FindAll() ?? Enumerable.Empty<DeploymentDbObject>())
                .Concat(GetEosSessions().Select(EosSessionToDeployment))
                .ToList();
            if (!includeOffline)
                deps = deps.Where(d => d.Online).ToList();

            // Dedupe by deployment id (a registered server also shows as an EOS session).
            var seen = new HashSet<string>();
            var items = new List<FleetStationResponse>();
            foreach (var d in deps)
                if (!string.IsNullOrEmpty(d.DeploymentId) && seen.Add(d.DeploymentId))
                    items.Add(DeploymentToFleetStation(d));

            return Results.Ok(new StationsPageV2 { FleetId = "", Items = items });
        }

        // GET /v1/region — the client geo-picks its region from this on boot. Self-host => fixed NA/US.
        [HttpGet("/v1/region")]
        public async Task<IHttpActionResult> GetRegion(IHttpRequest request, IHttpResponse response)
            => Results.Ok(new RegionResponse { ContinentCode = "NA", CountryCode = "US" });

        // POST /v2/users/log_in — player login (v2). Same user-upsert + key-mint + nonce handling as the
        // v0 path, but returns the SLIM v2 response (no embedded server list; client uses /v2/stations).
        [HttpPost("/v2/users/log_in")]
        public async Task<IHttpActionResult> UserPlayerLoginV2(IHttpRequest request, IHttpResponse response)
        {
            ClientLoginRequest? body;
            try { body = JsonSerializer.Deserialize<ClientLoginRequest>(request.Body); }
            catch { body = null; }

            if (body == null || string.IsNullOrEmpty(body.PlatformId))
                return Results.Ok(new ClientLoginV2Response { Success = false });

            if (NonceVerify)
            {
                var nonceOk = await VerifyNonceAny(body.PlatformId, body.Nonce);
                Console.WriteLine($"[A2DB][LOGIN v2] platformId='{body.PlatformId}' nonceOk={nonceOk} enforce={NonceEnforce}");
                if (NonceEnforce && !nonceOk)
                    return Results.Ok(new ClientLoginV2Response { Success = false });
            }

            var userId = body.PlatformId;
            var userCollection = Program.Database.GetCollection<UserDataResponse>(true);
            var user = userCollection?.FindOne(u => u.UserId == userId);
            if (user == null)
            {
                user = new UserDataResponse
                {
                    UserId = userId, Username = body.Username, Platform = body.Platform,
                    CreatedAt = DateTime.Now, LastLogin = DateTime.Now,
                    Roles = new List<string>(), Bans = new List<BanRequest>()
                };
                userCollection?.Insert(user);
            }
            else
            {
                user.Username = body.Username; user.Platform = body.Platform; user.LastLogin = DateTime.Now;
                userCollection?.Update(user);
            }

            var apiKey = GenerateApiKey(userId);
            var keyCollection = Program.Database.GetCollection<UserApiKeyDbObject>(true);
            keyCollection?.Insert(new UserApiKeyDbObject { UserId = userId, ApiKey = apiKey });

            return Results.Ok(new ClientLoginV2Response { Success = true, ApiKey = apiKey, OrgScopedId = userId });
        }

        // POST /v1/users/log_in_with_key — the 29932 server authenticates here (master key). Same as the
        // v0 route; alias so both builds' clients/servers resolve it.
        [HttpPost("/v1/users/log_in_with_key")]
        public Task<IHttpActionResult> LogInWithKeyV1(IHttpRequest request, IHttpResponse response)
            => LogInWithKey(request, response);

        // ─── DEPLOYMENTS ──────────────────────────────────────────────────────────

        // POST /stations/{station_id}/deployments — create deployment
        [HttpPost("/stations/{station_id}/deployments")]
        public async Task<IHttpActionResult> CreateDeployment(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            DeploymentRequest? body;
            try { body = JsonSerializer.Deserialize<DeploymentRequest>(request.Body); }
            catch { body = null; }

            var deploymentId = GenerateId();
            var apiKey = GenerateApiKey(deploymentId, "service");

            var deployment = new DeploymentDbObject
            {
                DeploymentId = deploymentId,
                StationId = station_id,
                DeploymentName = body?.DeploymentName,
                IpAddress = body?.IpAddress,
                Region = body?.Region,
                Version = body?.Version,
                CreatedAt = DateTime.Now,
                Online = false,
                PlayerCount = 0,
                Config = new Dictionary<string, string>(),
                ApiKeys = new List<string> { apiKey }
            };

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            deploymentCollection?.Insert(deployment);

            return Results.Ok(new CreateDeploymentResponse { ApiKey = apiKey, DeploymentId = deploymentId });
        }

        // GET /stations/{station_id}/deployments (deprecated v0)
        [HttpGet("/stations/{station_id}/deployments")]
        public async Task<IHttpActionResult> GetStationDeploymentsV0(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployments = (deploymentCollection?.Find(d => d.StationId == station_id) ?? Enumerable.Empty<DeploymentDbObject>())
                .Concat(GetEosSessions()
                    .Where(s => (s.StationId ?? s.Bucket) == station_id)
                    .Select(EosSessionToDeployment))
                .Select(DeploymentToResponse).ToList();

            return Results.Ok(new StationDeploymentResponse
            {
                StationId = station_id,
                Deployments = deployments.Select(d => new DeploymentExpanded
                {
                    StationId = d.StationId,
                    DeploymentName = d.DeploymentName,
                    IpAddress = d.IpAddress,
                    Region = d.Region,
                    Version = d.Version,
                    CreatedAt = d.CreatedAt,
                    Online = d.Online,
                    LastEvent = d.LastEventAt,
                    PlayerCount = d.PlayerCount,
                    Config = d.Config
                }).ToList()
            });
        }

        // GET /v1/stations/{station_id}/deployments
        [HttpGet("v1/stations/{station_id}/deployments")]
        public async Task<IHttpActionResult> GetStationDeploymentsV1(
            IHttpRequest request, IHttpResponse response, string station_id)
        {
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "500") ?? "500");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");
            bool includeOffline = bool.Parse(request.GetQueryParameter("include_offline", "false") ?? "false");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployments = (deploymentCollection?.Find(d => d.StationId == station_id) ?? Enumerable.Empty<DeploymentDbObject>())
                .Concat(GetEosSessions()
                    .Where(s => (s.StationId ?? s.Bucket) == station_id)
                    .Select(EosSessionToDeployment))
                .ToList();

            if (!includeOffline)
                deployments = deployments.Where(d => d.Online).ToList();

            var items = deployments.Select(DeploymentToResponse).ToList();
            var paged = PagedResponse<ServerDeploymentResponse>.Create<ServerDeploymentResponsePage>(items, pageSize, page);

            return Results.Ok(new DeploymentsResponsePage
            {
                StationId = station_id,
                Page = paged.Page,
                Items = paged.Items.Select(d => new DeploymentExpanded
                {
                    StationId = d.StationId,
                    DeploymentName = d.DeploymentName,
                    IpAddress = d.IpAddress,
                    Region = d.Region,
                    Version = d.Version,
                    CreatedAt = d.CreatedAt,
                    Online = d.Online,
                    LastEvent = d.LastEventAt,
                    PlayerCount = d.PlayerCount,
                    Config = d.Config
                }).ToList()
            });
        }

        // POST /deployments/{deployment_id}/api_key
        [HttpPost("/deployments/{deployment_id}/api_key")]
        public async Task<IHttpActionResult> CreateDeploymentApiKey(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
                return Results.Ok(new ApiKeyResponse { ApiKey = "" });

            var newKey = GenerateApiKey(deployment_id, "service");
            deployment.ApiKeys ??= new List<string>();
            deployment.ApiKeys.Add(newKey);
            deploymentCollection?.Update(deployment);

            return Results.Ok(new ApiKeyResponse { ApiKey = newKey });
        }

        // POST /deployments/{deployment_id}/server_events — add server event
        [HttpPost("/deployments/{deployment_id}/server_events")]
        public async Task<IHttpActionResult> AddServerEvent(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            ServerEventRequest? body;
            try { body = JsonSerializer.Deserialize<ServerEventRequest>(request.Body); }
            catch { body = null; }

            if (body == null || string.IsNullOrEmpty(body.EventType))
                return Results.Ok(new SuccessBoolean { Success = false });

            // Keep the latest full netvar/state dump for the dashboard's netvar editor (in memory, disk
            // throttled to 1/min) -- the size filter below still keeps it out of LiteDB.
            NetvarDumpStore.Record(deployment_id, body.EventType, body.EventData);

            // Only persist small events. The game fires "netvars" (~1.6 MB) and "state" (~40 KB)
            // every few seconds; storing those would bloat production.db fast, and we don't read
            // them back. Keep tiny events for monitoring, skip the big spammy ones.
            const int MaxStoredEventBytes = 8192;
            if (string.IsNullOrEmpty(body.EventData) || body.EventData.Length <= MaxStoredEventBytes)
            {
                var eventCollection = Program.Database.GetCollection<ServerEventDbObject>(true);
                eventCollection?.Insert(new ServerEventDbObject
                {
                    DeploymentId = deployment_id,
                    EventType = body.EventType,
                    EventData = body.EventData,
                    Timestamp = DateTime.Now
                });
            }

            // Update online status if this deployment has a real DB row. The Halcyon server uses a
            // synthetic deployment id ("halcyon") with no row — that's fine, we still ACK success so
            // the game's reporting delegate keeps ticking (returning false made it look rejected).
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment != null)
            {
                deployment.Online = true;
                deployment.LastEvent = DateTime.Now;
                deploymentCollection?.Update(deployment);

                var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
                var station = stationCollection?.FindOne(s => s.StationId == deployment.StationId);
                if (station != null)
                {
                    station.Online = true;
                    station.LastOnline = DateTime.Now;
                    stationCollection?.Update(station);
                }
            }

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // PATCH /deployments/{deployment_id} — update deployment info
        [HttpPatch("/deployments/{deployment_id}")]
        public async Task<IHttpActionResult> UpdateDeployment(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            DeploymentRequest? body;
            try { body = JsonSerializer.Deserialize<DeploymentRequest>(request.Body); }
            catch { body = null; }

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            if (body != null)
            {
                if (body.DeploymentName != null) deployment.DeploymentName = body.DeploymentName;
                if (body.IpAddress != null) deployment.IpAddress = body.IpAddress;
                if (body.Region != null) deployment.Region = body.Region;
                if (body.Version != null) deployment.Version = body.Version;
            }
            deploymentCollection?.Update(deployment);
            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // DELETE /deployments/{deployment_id}
        [HttpDelete("/deployments/{deployment_id}")]
        public async Task<IHttpActionResult> DeleteDeployment(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
                return Results.Ok(new SuccessBoolean { Success = false });

            deploymentCollection!.Delete(deployment.Id);

            // delete all server events for this deployment
            var eventCollection = Program.Database.GetCollection<ServerEventDbObject>(true);
            foreach (var ev in eventCollection?.Find(e => e.DeploymentId == deployment_id).ToList() ?? new())
                eventCollection!.Delete(ev.Id);

            return Results.Ok(new SuccessBoolean { Success = true });
        }

        // GET /deployments/{deployment_id}/config (deprecated v0)
        [HttpGet("/deployments/{deployment_id}/config")]
        public async Task<IHttpActionResult> GetDeploymentConfigV0(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            bool includeStation = bool.Parse(request.GetQueryParameter("include_station_config", "true") ?? "true");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id)
                ?? GetEosSessions().Select(EosSessionToDeployment).FirstOrDefault(d => d.DeploymentId == deployment_id);

            var config = deployment != null
                ? new Dictionary<string, string>(deployment.Config)
                : new Dictionary<string, string>();

            if (includeStation)
            {
                if (deployment != null)
                {
                    var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
                    var station = stationCollection?.FindOne(s => s.StationId == deployment.StationId);
                    if (station != null)
                        foreach (var kv in station.Config)
                            if (!kv.Key.StartsWith(NetvarOverridePrefix))   // overrides go via netvar_overrides, not here
                                config.TryAdd(kv.Key, kv.Value);
                }
                // Halcyon board/sign URLs (served even for an unknown/fixed deployment id).
                foreach (var kv in DefaultStationConfig)
                    config.TryAdd(kv.Key, kv.Value);
                AddBoardDefaults(config, request.Host);
            }

            return Results.Ok(config);
        }

        // GET /v1/deployments/{deployment_id}
        [HttpGet("v1/deployments/{deployment_id}")]
        public async Task<IHttpActionResult> GetDeploymentV1(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            bool includeDeploymentConfig = bool.Parse(request.GetQueryParameter("include_deployment_config", "true") ?? "true");
            bool includeStationConfig = bool.Parse(request.GetQueryParameter("include_station_config", "true") ?? "true");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
            {
                var eos = GetEosSessions().FirstOrDefault(s => s.Id == deployment_id);
                if (eos != null)
                    deployment = EosSessionToDeployment(eos);
                else
                {
                    // Synthetic deployment so a FIXED launch arg (-DashboardDeploymentId=halcyon)
                    // still resolves — the server mints its real deployment id at runtime, but the
                    // cmdline id is read at startup, so they won't match. CRITICAL: carry a REAL
                    // station id, else the game logs "Station ID not set" and never fetches roles
                    // (and our join-gate never triggers). Resolve the station from the most-recent
                    // online deployment register_server actually created. (FindAll().Where — never
                    // FindOne(captured) — per the LiteDB translator-hang gotcha.)
                    var latest = deploymentCollection?.FindAll()
                        .Where(d => d.Online && !string.IsNullOrEmpty(d.StationId))
                        .OrderByDescending(d => d.CreatedAt)
                        .FirstOrDefault();
                    deployment = new DeploymentDbObject
                    {
                        DeploymentId = deployment_id,
                        StationId = latest?.StationId ?? "",
                        DeploymentName = latest?.DeploymentName ?? "Halcyon",
                        Region = latest?.Region ?? "NAE", IpAddress = latest?.IpAddress ?? "",
                        Version = BuildVersion, CreatedAt = DateTime.Now,
                        Online = true, PlayerCount = 0, Config = new Dictionary<string, string>()
                    };
                }
            }

            var resp = DeploymentToResponse(deployment);
            // Build config from flags. Station config (incl. the Halcyon board defaults) is served
            // whenever include_station_config=true, independent of include_deployment_config.
            resp.Config ??= new Dictionary<string, string>();
            if (!includeDeploymentConfig)
                resp.Config = new Dictionary<string, string>();
            if (includeStationConfig)
            {
                var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
                var station = stationCollection?.FindOne(s => s.StationId == deployment.StationId);
                if (station != null)
                    foreach (var kv in station.Config)
                        if (!kv.Key.StartsWith(NetvarOverridePrefix))   // overrides go via netvar_overrides, not here
                            resp.Config.TryAdd(kv.Key, kv.Value);
                // Halcyon board/sign URLs (a real station config, if present, overrides these).
                foreach (var kv in DefaultStationConfig)
                    resp.Config.TryAdd(kv.Key, kv.Value);
                AddBoardDefaults(resp.Config, request.Host);
            }

            return Results.Ok(resp);
        }

        // GET /v1/deployments/{deployment_id}/netvar_overrides
        // Polled over loopback by the injected DLL (~every 10s). Deliberately plain text, not JSON, so the
        // C++ side needs no parser:
        //   v=<sha1 of the lines below>          -- unchanged hash => the DLL does nothing
        //   module<TAB><SlotID|*><TAB><Variable><TAB><value>
        //   world<TAB><TAB><netvar path><TAB><value>
        [HttpGet("v1/deployments/{deployment_id}/netvar_overrides")]
        public async Task<IHttpActionResult> GetNetvarOverrides(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            string stationId = deployment?.StationId ?? "";
            if (string.IsNullOrEmpty(stationId))
            {
                // Same fallback as GetDeploymentV1: a fixed launch id resolves to the station of the most
                // recent online deployment. (FindAll().Where -- never FindOne(captured) -- LiteDB gotcha.)
                var latest = deploymentCollection?.FindAll()
                    .Where(d => d.Online && !string.IsNullOrEmpty(d.StationId))
                    .OrderByDescending(d => d.CreatedAt)
                    .FirstOrDefault();
                stationId = latest?.StationId ?? "";
            }
            var station = string.IsNullOrEmpty(stationId) ? null
                : Program.Database.GetCollection<StationDbObject>(true)?.FindOne(s => s.StationId == stationId);

            // The game server calls this once at startup; make sure the watched files exist too.
            NetvarOverridesFile.EnsureAllWritten();
            return Results.Configurable(HttpStatusCode.OK, "text/plain; charset=utf-8",
                System.Text.Encoding.UTF8.GetBytes(NetvarOverridesFile.BuildText(station)));
        }

        // GET /v1/deployments/{deployment_id}/config
        [HttpGet("v1/deployments/{deployment_id}/config")]
        public async Task<IHttpActionResult> GetDeploymentConfigV1(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            bool includeStation = bool.Parse(request.GetQueryParameter("include_station_config", "true") ?? "true");
            bool includeEvent = bool.Parse(request.GetQueryParameter("include_event_config", "true") ?? "true");

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);

            var config = deployment != null
                ? new Dictionary<string, string>(deployment.Config)
                : new Dictionary<string, string>();

            if (includeStation)
            {
                if (deployment != null)
                {
                    var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
                    var station = stationCollection?.FindOne(s => s.StationId == deployment.StationId);
                    if (station != null)
                        foreach (var kv in station.Config)
                            if (!kv.Key.StartsWith(NetvarOverridePrefix))   // overrides go via netvar_overrides, not here
                                config.TryAdd(kv.Key, kv.Value);
                }
                // Halcyon board/sign URLs (served even for an unknown/fixed deployment id).
                foreach (var kv in DefaultStationConfig)
                    config.TryAdd(kv.Key, kv.Value);
                AddBoardDefaults(config, request.Host);
            }

            if (includeEvent)
            {
                var eventCollection = Program.Database.GetCollection<StationEventDbObject>(true);
                var now = DateTime.Now;
                var activeEvent = eventCollection?.FindAll()
                    .Where(e => e.StationId == deployment.StationId)
                    .Where(e => e.DeploymentId == deployment_id || string.IsNullOrEmpty(e.DeploymentId))
                    .Where(e => e.StartTime <= now && e.StartTime.AddSeconds(e.Duration) >= now)
                    .FirstOrDefault();
                if (activeEvent != null)
                    foreach (var kv in activeEvent.Config)
                        config.TryAdd(kv.Key, kv.Value);
            }

            return Results.Ok(config);
        }

        // POST /v1/deployments/{deployment_id}/config — merge keys
        [HttpPost("v1/deployments/{deployment_id}/config")]
        public async Task<IHttpActionResult> SetDeploymentConfigV1(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            Dictionary<string, string>? data;
            try { data = JsonSerializer.Deserialize<Dictionary<string, string>>(request.Body); }
            catch { data = null; }

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
                return Results.Ok(new Dictionary<string, string>());

            if (data != null)
                foreach (var kv in data)
                    deployment.Config[kv.Key] = kv.Value;

            deploymentCollection?.Update(deployment);
            return Results.Ok(deployment.Config);
        }

        // DELETE /v1/deployments/{deployment_id}/config — remove keys
        [HttpDelete("v1/deployments/{deployment_id}/config")]
        public async Task<IHttpActionResult> DeleteDeploymentConfigV1(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            List<string>? keys;
            try { keys = JsonSerializer.Deserialize<List<string>>(request.Body); }
            catch { keys = null; }

            var deploymentCollection = Program.Database.GetCollection<DeploymentDbObject>(true);
            var deployment = deploymentCollection?.FindOne(d => d.DeploymentId == deployment_id);
            if (deployment == null)
                return Results.Ok(new Dictionary<string, string>());

            if (keys != null)
                foreach (var k in keys)
                    deployment.Config.Remove(k);

            deploymentCollection?.Update(deployment);
            return Results.Ok(deployment.Config);
        }

        // GET /v1/deployments/{deployment_id}/server_events
        [HttpGet("v1/deployments/{deployment_id}/server_events")]
        public async Task<IHttpActionResult> GetDeploymentServerEvents(
            IHttpRequest request, IHttpResponse response, string deployment_id)
        {
            int pageSize = int.Parse(request.GetQueryParameter("page_size", "500") ?? "500");
            int page = int.Parse(request.GetQueryParameter("page", "1") ?? "1");
            string? eventType = request.GetQueryParameter("event_type");

            var eventCollection = Program.Database.GetCollection<ServerEventDbObject>(true);
            var query = eventCollection?.FindAll()
                .Where(e => e.DeploymentId == deployment_id) ?? Enumerable.Empty<ServerEventDbObject>();

            if (!string.IsNullOrEmpty(eventType))
                query = query.Where(e => e.EventType == eventType);

            var allEvents = query.OrderByDescending(e => e.Timestamp).ToList();
            var mapped = allEvents.Select((e, i) => new ServerEventResponse
            {
                Index = i + 1,
                EventType = e.EventType,
                DeploymentId = e.DeploymentId,
                EventData = e.EventData,
                Timestamp = e.Timestamp
            }).ToList();

            return Results.Ok(PagedResponse<ServerEventResponse>.Create<ServerEventResponsePage>(mapped, pageSize, page));
        }
    }
}
