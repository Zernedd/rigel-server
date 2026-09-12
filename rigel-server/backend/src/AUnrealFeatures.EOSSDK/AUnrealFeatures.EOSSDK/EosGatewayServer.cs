using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.EOSSDK.Models;
using System.Net;
using System.Reflection;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.EOSSDK;

public interface IEosGatewayServer { }

public sealed partial class EosGatewayServer : AstraHttpServer, IEosGatewayServer
{
    const string HOSTNAME = "localhost";
    const ushort PORT = 50;

    const string PRODUCT_ID    = "472bc2b0df5c4a78921dbc8d6d70b7e7";
    const string SANDBOX_ID    = "2a166d77d93144289a40283f5d1d70ca";
    internal const string DEPLOYMENT_ID = "e203ea2f9d70483fb6149107d52c25b5";
    internal static string DeploymentId => DEPLOYMENT_ID;
    const string ORG_ID        = "o-p2vxwcxe6x6wv45luacent56lxbgun";
    const string CLIENT_ID     = "xyza7891wH5ksxQFbH850JkfRrnz4RGe";

    const string CORRELATION_ID = "EOS-fiWTTmfxAk6W-hQjcUin0A-08vSZf-Sjk6Xbx4FA3ewVg";
    const string SERVER_NAME    = "eos-gateway";
    static readonly JsonElement DefaultEndpointsPayload;
    static readonly JsonElement CustomEndpointsPayload;
    static readonly JsonDocument _defaultDoc;
    static readonly JsonDocument _customDoc;

    // ── PUID → display name registry (populated on login) ────────────────────
    internal static readonly System.Collections.Concurrent.ConcurrentDictionary<string, string> _userNames = new();

    // ── Dynamic EOS session list — managed via DashboardServer /api/sessions ──
    public static readonly List<EosSessionInfo> Sessions = new();
    static readonly object _sessionLock = new();

    // ── Per-station whitelists ───────────────────────────────────────────────
    // A station with a non-empty whitelist only appears in the station browser for the accounts on it.
    // This project cannot reference Ares (Ares -> EOSSDK, not the reverse), so the values are PUSHED in
    // from the Ares side whenever a station's config changes (see StationAcl). Station ids and names are
    // both matched case-insensitively.
    static readonly System.Collections.Concurrent.ConcurrentDictionary<string, HashSet<string>> _stationWhitelist =
        new(StringComparer.OrdinalIgnoreCase);

    public static void SetStationWhitelist(string? stationId, IEnumerable<string>? usernames)
    {
        if (string.IsNullOrWhiteSpace(stationId)) return;
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var u in usernames ?? Enumerable.Empty<string>())
            if (!string.IsNullOrWhiteSpace(u)) set.Add(u.Trim());
        if (set.Count == 0) _stationWhitelist.TryRemove(stationId!, out _);
        else _stationWhitelist[stationId!] = set;
    }

    public static int WhitelistedStationCount => _stationWhitelist.Count;

    // Decode the caller out of the EOS user token. We minted and signed it ourselves (EosJwtFactory), so the
    // payload is trusted here; it is only ever used to decide what to SHOW, never to grant anything.
    static byte[] Base64UrlDecode(string s)
    {
        string t = s.Replace('-', '+').Replace('_', '/');
        switch (t.Length % 4) { case 2: t += "=="; break; case 3: t += "="; break; }
        return Convert.FromBase64String(t);
    }

    internal static (string Name, string Puid) IdentifyCaller(IHttpRequest request)
    {
        try
        {
            var header = request.GetHeaderValue("Authorization") ?? "";
            if (!header.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase)) return ("", "");
            var parts = header[7..].Trim().Split('.');
            if (parts.Length < 2) return ("", "");
            using var doc = JsonDocument.Parse(Encoding.UTF8.GetString(Base64UrlDecode(parts[1])));
            var root = doc.RootElement;
            string puid = root.TryGetProperty("productUserId", out var p) ? p.GetString() ?? "" : "";
            string name = "";
            if (root.TryGetProperty("account", out var acc) && acc.ValueKind == JsonValueKind.Object &&
                acc.TryGetProperty("displayName", out var dn))
                name = dn.GetString() ?? "";
            if (string.IsNullOrEmpty(name) && !string.IsNullOrEmpty(puid) && _userNames.TryGetValue(puid, out var known))
                name = known;
            return (name, puid);
        }
        catch { return ("", ""); }
    }

    // An allowlist fails CLOSED: a station that has one stays hidden from a caller we cannot identify.
    // Stations without a whitelist are untouched, so this changes nothing for everyone else.
    static bool VisibleTo(EosSessionInfo s, string name, string puid)
    {
        if (string.IsNullOrWhiteSpace(s.StationId)) return true;
        if (!_stationWhitelist.TryGetValue(s.StationId!, out var allowed) || allowed.Count == 0) return true;
        if (!string.IsNullOrEmpty(name) && allowed.Contains(name)) return true;
        // The Oculus login derives the puid straight from the display name, so a whitelisted name still
        // matches when the token carries no readable displayName.
        if (!string.IsNullOrEmpty(puid))
            foreach (var n in allowed)
                if (string.Equals(EosJwtFactory.ProductUserIdFor(n), puid, StringComparison.OrdinalIgnoreCase))
                    return true;
        return false;
    }

    // Runtime-registered sessions are otherwise lost on restart (only re-seeded from the baked
    // matchmaking.json), so gameservers/deployments disappear every time the backend bounces.
    // Persist them to disk next to the binary and reload on boot — same pattern as lobbies.json.
    static readonly string _sessionsPath = Path.Combine(AppContext.BaseDirectory, "sessions.json");

    internal static void PersistSessions()
    {
        try
        {
            List<EosSessionInfo> toSave;
            lock (_sessionLock) toSave = Sessions.ToList();
            File.WriteAllText(_sessionsPath, JsonSerializer.Serialize(toSave, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { /* persistence failure is non-fatal */ }
    }

    static void LoadPersistedSessions()
    {
        try
        {
            if (!File.Exists(_sessionsPath)) return;
            var list = JsonSerializer.Deserialize<List<EosSessionInfo>>(File.ReadAllText(_sessionsPath));
            if (list == null) return;
            lock (_sessionLock)
            {
                foreach (var s in list)
                {
                    Sessions.RemoveAll(x => x.Id == s.Id); // upsert — persisted state overrides the baked seed
                    Sessions.Add(s);
                }
            }
            Console.WriteLine($"[EosGatewayServer] Loaded {list.Count} persisted sessions from disk");
        }
        catch { /* non-fatal */ }
    }

    // ── Dynamic EOS lobby list — player-hosted rooms ──────────────────────────
    internal static readonly List<EosLobbySession> Lobbies = new();
    internal static readonly object LobbyLock = new();

    static readonly string _lobbiesPath = Path.Combine(AppContext.BaseDirectory, "lobbies.json");

    internal static void PersistLobbies()
    {
        try
        {
            List<EosLobbySession> toSave;
            lock (LobbyLock)
                toSave = Lobbies.Where(l => l.Attributes.ContainsKey("ADDRESS_s")).ToList();
            File.WriteAllText(_lobbiesPath, JsonSerializer.Serialize(toSave, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { /* persistence failure is non-fatal */ }
    }

    static void LoadPersistedLobbies()
    {
        try
        {
            if (!File.Exists(_lobbiesPath)) return;
            var list = JsonSerializer.Deserialize<List<EosLobbySession>>(File.ReadAllText(_lobbiesPath));
            if (list == null) return;
            lock (LobbyLock) { Lobbies.AddRange(list); }
            Console.WriteLine($"[EosGatewayServer] Loaded {list.Count} persisted lobbies from disk");
        }
        catch { /* non-fatal */ }
    }

    // ── Stale-lobby reaper ────────────────────────────────────────────────────
    // Player-hosted lobbies send a "heartbeat" WS frame on an interval (which bumps
    // LastUpdated). When the host crashes / loses connection it stops heartbeating,
    // so a lobby that goes silent past LOBBY_TTL is considered dead and removed.
    // Infra-managed dedicated-server lobbies (those carrying ADDRESS_s) are never
    // reaped — they are owned by the matchmaker, not by a live WS connection.
    static readonly TimeSpan LOBBY_TTL    = TimeSpan.FromSeconds(90);
    static readonly TimeSpan LOBBY_SWEEP  = TimeSpan.FromSeconds(30);
    static System.Threading.Timer? _lobbyReaper;

    static void StartLobbyReaper()
    {
        _lobbyReaper = new System.Threading.Timer(_ =>
        {
            try
            {
                var now = DateTime.UtcNow;
                List<EosLobbySession> expired = new();
                lock (LobbyLock)
                {
                    foreach (var l in Lobbies)
                    {
                        if (l.Attributes.ContainsKey("ADDRESS_s")) continue; // infra-managed, never reap
                        if (!DateTime.TryParse(l.LastUpdated, null,
                                System.Globalization.DateTimeStyles.AdjustToUniversal |
                                System.Globalization.DateTimeStyles.AssumeUniversal, out var last))
                            continue; // no parseable timestamp → leave it alone
                        if (now - last > LOBBY_TTL) expired.Add(l);
                    }
                    foreach (var l in expired) Lobbies.Remove(l);
                }

                foreach (var l in expired)
                {
                    Console.WriteLine($"[EosGatewayServer] Reaped stale lobby {l.Id} (silent > {LOBBY_TTL.TotalSeconds:F0}s)");
                    _ = LobbyWsHelper.BroadcastAsync(l.Id, new
                    {
                        name    = "lobbydeleted",
                        payload = new { lobbyId = l.Id, name = "lobbydeleted" }
                    });
                }

                if (expired.Count > 0) PersistLobbies();
            }
            catch { /* sweep failure is non-fatal */ }
        }, null, LOBBY_SWEEP, LOBBY_SWEEP);
    }

    public static void UpsertSession(EosSessionInfo session)
    {
        lock (_sessionLock)
        {
            Sessions.RemoveAll(s => s.Id == session.Id);
            Sessions.Add(session);
        }
        PersistSessions();
    }

    public static bool RemoveSession(string id)
    {
        bool removed;
        lock (_sessionLock) { removed = Sessions.RemoveAll(s => s.Id == id) > 0; }
        if (removed) PersistSessions();
        return removed;
    }

    // Drop every matchmaking session belonging to one deployment. The station list the game browses is
    // built from these, so this is what actually takes a dead server out of the browser -- a crashed or
    // frozen server never deletes its own session, and players keep being sent to it. Called by the
    // watchdog the moment it is certain a server is down, before the replacement is launched.
    // Returns the number removed.
    public static int RemoveSessionsByDeployment(string deploymentId)
    {
        if (string.IsNullOrWhiteSpace(deploymentId)) return 0;
        int n;
        lock (_sessionLock) { n = Sessions.RemoveAll(s => s.DeploymentId == deploymentId); }
        if (n > 0) PersistSessions();
        return n;
    }

    // Prune matchmaking sessions down to a set of deployment ids worth keeping (the live ones). Crash
    // loops before the watchdog left dozens of ghost sessions whose game server is long gone; they clog
    // the station browser and inflate player counts. Keep only sessions whose deployment is in
    // keepDeploymentIds; drop the rest. Returns the number removed.
    public static int PruneSessionsExcept(HashSet<string> keepDeploymentIds)
    {
        int removed;
        lock (_sessionLock)
        {
            var before = Sessions.Count;
            // Keep exactly ONE session per live deployment -- the LAST (newest) one, since register_server
            // appends. Everything else goes: duplicates of a live deployment (a server that re-registered
            // across restarts left a trail) and every session of a deployment that is not in the keep set
            // (dead). This is what actually clears the ghost pile; removing only by deployment kept all the
            // duplicates of the one live server.
            var lastByDep = new Dictionary<string, EosSessionInfo>();
            foreach (var s in Sessions)
                if (!string.IsNullOrEmpty(s.DeploymentId) && keepDeploymentIds.Contains(s.DeploymentId!))
                    lastByDep[s.DeploymentId!] = s;   // later entries overwrite -> keeps the newest
            var survivors = new HashSet<EosSessionInfo>(lastByDep.Values);
            Sessions.RemoveAll(s => !survivors.Contains(s));
            removed = before - Sessions.Count;
        }
        if (removed > 0) PersistSessions();
        return removed;
    }

    // Wipe every matchmaking session (used by the admin station purge — the DB purge alone leaves these
    // in-memory sessions, which is what the game's station listing actually reads, so stations linger).
    // Returns the number removed.
    public static int ClearSessions()
    {
        int n;
        lock (_sessionLock)
        {
            n = Sessions.Count;
            Sessions.Clear();
        }
        PersistSessions();
        return n;
    }

    // Reconcile a session's player count to the AUTHORITATIVE value the gameserver reports (its live
    // netdriver ClientConnections count). PublicPlayers is otherwise only mutated by clients, so hard
    // disconnects (no leave DELETE) leave ghosts and the count never drops. The count is what's shown
    // (TotalPlayers/OpenPublicPlayers derive from PublicPlayers.Count), so trim the list down to the
    // reported count — dropping the oldest entries, the most-likely stale ones. Returns true if a
    // matching session was found. We never fabricate ids when the report is higher (client joins add
    // the real ids); we only prune excess.
    public static bool SetSessionPlayerCount(string deploymentId, int count)
    {
        if (string.IsNullOrWhiteSpace(deploymentId)) return false;
        if (count < 0) count = 0;
        lock (_sessionLock)
        {
            var s = Sessions.FirstOrDefault(x => x.DeploymentId == deploymentId);
            if (s == null) return false;
            while (s.PublicPlayers.Count > count)
                s.PublicPlayers.RemoveAt(0);
        }
        PersistSessions();
        return true;
    }

    static EosGatewayServer()
    {
        var asm = Assembly.GetExecutingAssembly();
        _defaultDoc = JsonDocument.Parse(ReadResource(asm, "default_endpoints.json"));
        _customDoc  = JsonDocument.Parse(ReadResource(asm, "custom_endpoints.json"));

        DefaultEndpointsPayload = _defaultDoc.RootElement;
        CustomEndpointsPayload  = _customDoc.RootElement;

        // Seed Sessions from matchmaking.json
        try
        {
            var json = ReadResource(asm, "matchmaking.json");
            var doc  = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("sessions", out var arr))
            {
                foreach (var s in arr.EnumerateArray())
                {
                    var id           = s.TryGetProperty("id",         out var v)  ? v.GetString()  ?? "" : "";
                    var deploymentId = s.TryGetProperty("deployment", out var dv) ? dv.GetString() ?? "" : "";
                    var bucket       = s.TryGetProperty("bucket",     out var b)  ? b.GetString()  ?? "A2_4729" : "A2_4729";
                    int maxP   = s.TryGetProperty("settings", out var settings) &&
                                 settings.TryGetProperty("maxPublicPlayers", out var mp)
                                 ? mp.GetInt32() : 10;
                    string svrName = "", ip = "", port = "", imguiPort = "";
                    int buildId = 4729;
                    if (s.TryGetProperty("attributes", out var attrs))
                    {
                        if (attrs.TryGetProperty("SERVERNAME_s",    out var sn)) svrName   = sn.GetString() ?? "";
                        if (attrs.TryGetProperty("ADDRESS_s",       out var ad)) ip        = ad.GetString() ?? "";
                        if (attrs.TryGetProperty("PUBLICPORT_s",    out var pp)) port      = pp.GetString() ?? "";
                        if (attrs.TryGetProperty("BUILDUNIQUEID_l", out var bi)) buildId   = bi.GetInt32();
                        if (attrs.TryGetProperty("IMGUIPORT_s",     out var ig)) imguiPort = ig.GetString() ?? "";
                    }
                    var publicPlayers = new List<string>();
                    if (s.TryGetProperty("publicPlayers", out var ppArr))
                        foreach (var p in ppArr.EnumerateArray())
                            if (p.GetString() is string pid) publicPlayers.Add(pid);
                    Sessions.Add(new EosSessionInfo
                    {
                        Id            = id,
                        DeploymentId  = string.IsNullOrEmpty(deploymentId) ? null : deploymentId,
                        ServerName    = svrName,
                        IpAddress     = ip,
                        Port          = port,
                        MaxPlayers    = maxP,
                        Bucket        = bucket,
                        BuildId       = buildId,
                        ImguiPort     = imguiPort,
                        PublicPlayers = publicPlayers,
                    });
                }
            }
        }
        catch { /* seed failure is non-fatal */ }

        // Reload runtime-registered sessions saved before the last restart (overrides the seed by id).
        LoadPersistedSessions();

        LoadPersistedLobbies();
        StartLobbyReaper();
    }

    static string ReadResource(Assembly asm, string filename)
    {
        var name = asm.GetManifestResourceNames()
            .FirstOrDefault(n => n.EndsWith(filename, StringComparison.OrdinalIgnoreCase));
        if (name == null) return "{}";
        using var stream = asm.GetManifestResourceStream(name)!;
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }

    public EosGatewayServer() : base(HOSTNAME, PORT) { }

    private static void AddEosHeaders(IHttpResponse response)
    {
        response.SetHeader("X-Epic-Correlation-Id", CORRELATION_ID);
        response.SetHeader("Server", SERVER_NAME);
    }

    // ── Auth ─────────────────────────────────────────────────────────────────

    // Anonymous device-based login (Quest / Android). Body is form-encoded.
    // Real Epic response is just { access_token }; we match that shape.
    // PUID is deterministic from deviceModel so the same device always gets the same account.
    [HttpPost("/auth/v1/accounts/deviceid")]
    public async Task<IHttpActionResult> DeviceIdLogin(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);

        var authHeader = request.GetHeaderValue("Authorization");
        var clientId   = EosJwtFactory.ExtractClientId(authHeader, CLIENT_ID);
        var clientIp   = request.Remote?.ToString() ?? "127.0.0.1";

        var form = request.FormBody;
        form.TryGetValue("deviceModel",  out var deviceKey);
        form.TryGetValue("displayName",  out var deviceName);
        form.TryGetValue("nonce",        out var nonce);
        deviceKey  = string.IsNullOrEmpty(deviceKey)  ? "anonymous" : deviceKey;
        deviceName = string.IsNullOrEmpty(deviceName) ? "Player"    : deviceName;
        nonce      = string.IsNullOrEmpty(nonce)      ? Guid.NewGuid().ToString("N") : nonce;

        // Nonce is generated once per device by the EOS SDK and stored in secure storage —
        // it's the only truly unique per-device identifier we receive.
        var uniqueKey   = $"device:{nonce}";
        var puid        = EosJwtFactory.ProductUserIdFor(uniqueKey);
        var ouid        = EosJwtFactory.OrgUserIdFor(uniqueKey);
        var accessToken = EosJwtFactory.UserToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, ORG_ID,
                                                   puid, ouid, nonce, deviceName, clientIp, idp: "deviceid", platformType: "other");

        _userNames[puid] = deviceName;
        Logger.Information($"DeviceId login | name={deviceName} device={deviceKey} puid={puid}");

        return Results.Ok(new { access_token = accessToken, token_type = "bearer" });
    }

    [HttpPost("/auth/v1/oauth/token")]
    public async Task<IHttpActionResult> OAuthToken(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);

        var form = request.FormBody;
        form.TryGetValue("grant_type",         out var grantType);
        form.TryGetValue("external_auth_type",  out var externalAuthType);
        form.TryGetValue("external_auth_token", out var externalAuthToken);
        form.TryGetValue("nonce",               out var nonce);
        form.TryGetValue("display_name",        out var displayName);

        request.Headers.TryGetValue("Authorization", out var authHeader);
        var clientId  = EosJwtFactory.ExtractClientId(authHeader, CLIENT_ID);
        var clientIp  = request.Remote?.ToString() ?? "127.0.0.1";
        // client_credentials gets a 1-hour token (matching real EOS); user tokens get 10 years
        var clientExpiresAt = DateTime.UtcNow.AddHours(1).ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
        var userExpiresAt   = DateTime.UtcNow.AddYears(10).ToString("yyyy-MM-ddTHH:mm:ss.fffZ");

        Logger.Information($"EOS OAuth | grant_type={grantType} external_auth_type={externalAuthType} display_name={displayName}");

        if (grantType == "external_auth" && externalAuthType == "steam_encrypted_appticket")
        {
            var steamId     = SteamIdFromTicket(externalAuthToken);
            var name        = string.IsNullOrEmpty(displayName) ? "Player" : displayName;
            var puid        = EosJwtFactory.ProductUserIdFor(steamId);
            var ouid        = EosJwtFactory.OrgUserIdFor(steamId);
            var accessToken = EosJwtFactory.UserToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, ORG_ID, puid, ouid, nonce ?? "", name, clientIp,
                                                      idp: "steam", platformId: steamId, platformType: "steam");
            var idToken     = EosJwtFactory.IdToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, puid,
                                                    idp: "steam", platformId: steamId, platformType: "steam");

            return Results.Ok(new EosOAuthTokenResponse
            {
                AccessToken          = accessToken,
                ExpiresAt            = userExpiresAt,
                ExpiresIn            = 315360000,
                Nonce                = nonce ?? "",
                Features             = EosJwtFactory.Features,
                OrganizationId       = ORG_ID,
                ProductId            = PRODUCT_ID,
                SandboxId            = SANDBOX_ID,
                DeploymentId         = DEPLOYMENT_ID,
                OrganizationUserId   = ouid,
                ProductUserId        = puid,
                ProductUserIdCreated = false,
                IdToken              = idToken,
                ClientId             = clientId,
            });
        }

        if (grantType == "external_auth" && externalAuthType == "deviceid_access_token")
        {
            // Derive PUID from the device JWT string itself — each device caches a unique JWT
            // (unique jti), so this is stable per device and avoids collisions from stale cached tokens.
            var puid        = EosJwtFactory.ProductUserIdFor("devicejwt:" + externalAuthToken);
            var name        = string.IsNullOrEmpty(displayName) ? "Player" : displayName;
            if (!string.IsNullOrEmpty(name) && name != "Player") _userNames[puid] = name;
            var ouid        = EosJwtFactory.OrgUserIdFor(puid);
            var accessToken = EosJwtFactory.UserToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, ORG_ID, puid, ouid, nonce ?? "", name, clientIp, idp: "deviceid", platformType: "other");
            var idToken     = EosJwtFactory.IdToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, puid, idp: "deviceid", platformType: "other");

            return Results.Ok(new EosOAuthTokenResponse
            {
                AccessToken          = accessToken,
                ExpiresAt            = userExpiresAt,
                ExpiresIn            = 315360000,
                Nonce                = nonce ?? "",
                Features             = EosJwtFactory.Features,
                OrganizationId       = ORG_ID,
                ProductId            = PRODUCT_ID,
                SandboxId            = SANDBOX_ID,
                DeploymentId         = DEPLOYMENT_ID,
                OrganizationUserId   = ouid,
                ProductUserId        = puid,
                ProductUserIdCreated = false,
                IdToken              = idToken,
                ClientId             = clientId,
            });
        }

        if (grantType == "external_auth" && externalAuthType == "oculus_userid_nonce")
        {
            var name        = string.IsNullOrEmpty(displayName) ? "Player" : displayName;
            var puid        = EosJwtFactory.ProductUserIdFor(name);
            var ouid        = EosJwtFactory.OrgUserIdFor(name);
            var accessToken = EosJwtFactory.UserToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, ORG_ID, puid, ouid, nonce ?? "", name, clientIp);
            var idToken     = EosJwtFactory.IdToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, puid);

            return Results.Ok(new EosOAuthTokenResponse
            {
                AccessToken          = accessToken,
                ExpiresAt            = userExpiresAt,
                ExpiresIn            = 315360000,
                Nonce                = nonce ?? "",
                Features             = EosJwtFactory.Features,
                OrganizationId       = ORG_ID,
                ProductId            = PRODUCT_ID,
                SandboxId            = SANDBOX_ID,
                DeploymentId         = DEPLOYMENT_ID,
                OrganizationUserId   = ouid,
                ProductUserId        = puid,
                ProductUserIdCreated = false,
                IdToken              = idToken,
                ClientId             = clientId,
            });
        }

        if (grantType == "external_auth")
            return Results.BadRequest(EosError("common.oauth.invalid_request",
                $"Unsupported external_auth_type: {externalAuthType}", 1015));

        if (!string.IsNullOrEmpty(grantType) && grantType != "client_credentials")
            return Results.BadRequest(EosError("common.oauth.invalid_grant",
                $"Unsupported grant_type: {grantType}", 1016));

        var clientToken = EosJwtFactory.ClientToken(clientId, PRODUCT_ID, SANDBOX_ID, DEPLOYMENT_ID, ORG_ID);
        return Results.Ok(new EosOAuthTokenResponse
        {
            AccessToken    = clientToken,
            ExpiresAt      = clientExpiresAt,
            ExpiresIn      = 3599,
            Features       = EosJwtFactory.Features,
            OrganizationId = ORG_ID,
            ProductId      = PRODUCT_ID,
            SandboxId      = SANDBOX_ID,
            DeploymentId   = DEPLOYMENT_ID,
        });
    }

    [HttpGet("/auth/v1/oauth/.well-known/jwks.json")]
    public async Task<IHttpActionResult> Jwks(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);
        return Results.Ok(EosJwtFactory.GetJwks());
    }

    const string TURN_SECRET = "pavlov_turn_secret_key_32chars!!";

    [HttpPost("/auth/v1/turn/credentials")]
    public async Task<IHttpActionResult> TurnCredentials(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);

        var puid    = ExtractPuidFromBearer(request.GetHeaderValue("Authorization"));
        var expiry  = DateTimeOffset.UtcNow.AddHours(1).ToUnixTimeSeconds();
        var username = $"{expiry}:{puid}";

        using var hmac = new System.Security.Cryptography.HMACSHA1(
            System.Text.Encoding.UTF8.GetBytes(TURN_SECRET));
        var hash     = hmac.ComputeHash(System.Text.Encoding.UTF8.GetBytes(username));
        var password = Convert.ToBase64String(hash);

        return Results.Ok(new EosTurnCredentialsResponse
        {
            Username = username,
            Password = password,
            Ttl      = 3600000,
            Uris     = new[]
            {
                "stun:94.72.120.104:3478",
                "turn:94.72.120.104:3478?transport=udp",
                "turn:94.72.120.104:3478?transport=tcp",
            }
        });
    }

    [HttpGet("/rtc/v1/{deployment}/room/{roomName}")]
    public async Task<IHttpActionResult> RtcRoomToken(IHttpRequest request, IHttpResponse response, string deployment, string roomName)
    {
        AddEosHeaders(response);
        var puid  = ExtractPuidFromBearer(request.GetHeaderValue("Authorization"));
        var token = RtcHelper.MakeToken(roomName, puid);
        return Results.Ok(new { token, url = RtcHelper.SERVER_URL });
    }

    // ── SDK Config ───────────────────────────────────────────────────────────

    [HttpGet("/sdk/v1/default")]
    public async Task<IHttpActionResult> DefaultEndpoints(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);
        return Results.Ok(DefaultEndpointsPayload);
    }

    [HttpGet("/sdk/v1/product/{product_id}")]
    public async Task<IHttpActionResult> CustomEndpoints(IHttpRequest request, IHttpResponse response, string product_id)
    {
        AddEosHeaders(response);
        Logger.Information($"SDK config requested for product: {product_id}");
        return Results.Ok(CustomEndpointsPayload);
    }

    // ── WebSocket (STOMP) — Notifications + Lobby ────────────────────────────

    protected override async Task<bool> HandleWebSocketAsync(System.Net.HttpListenerContext ctx)
    {
        var path = ctx.Request.Url?.AbsolutePath ?? "";

        if (LobbyWsHelper.IsLobbyPath(path))
        {
            Logger.Information($"WS[5000] lobby | {path}");
            var ownerPuid = ExtractPuidFromBearer(ctx.Request.Headers["Authorization"]);
            if (string.IsNullOrEmpty(ownerPuid) || ownerPuid == "0002000000000000000000000000000000")
            {
                ctx.Response.StatusCode = 401;
                ctx.Response.Close();
                return true;
            }
            var wsCtx = await ctx.AcceptWebSocketAsync(null);
            var ws    = wsCtx.WebSocket;
            try   { await LobbyWsHelper.RunAsync(ws, path, ownerPuid, msg => Logger.Information(msg)); }
            catch (Exception ex) { Logger.Error($"LobbyWS error | {ex.Message}"); }
            finally
            {
                if (ws.State == System.Net.WebSockets.WebSocketState.Open)
                    await ws.CloseAsync(System.Net.WebSockets.WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
                ws.Dispose();
            }
            return true;
        }

        if (!StompHelper.IsStompPath(path)) return false;

        Logger.Information($"WS[5000] stomp | {path}");
        var offered     = ctx.Request.Headers["Sec-WebSocket-Protocol"];
        var subprotocol = offered?.Split(',').Select(p => p.Trim()).FirstOrDefault() ?? "v12.stomp";
        var wsCtx2 = await ctx.AcceptWebSocketAsync(subprotocol);
        var ws2    = wsCtx2.WebSocket;
        try
        {
            await StompHelper.RunAsync(ws2, path, msg => Logger.Information(msg));
        }
        catch (Exception ex) { Logger.Error($"STOMP error | {ex.Message}"); }
        finally
        {
            if (ws2.State == System.Net.WebSockets.WebSocketState.Open)
                await ws2.CloseAsync(System.Net.WebSockets.WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
            ws2.Dispose();
        }
        return true;
    }

    // ── Matchmaking ──────────────────────────────────────────────────────────

    [HttpPost("/matchmaking/v1/{deployment_id}/filter")]
    public async Task<IHttpActionResult> MatchmakingFilter(IHttpRequest request, IHttpResponse response, string deployment_id)
    {
        AddEosHeaders(response);

        EosFilterRequest? filterReq = null;
        try { filterReq = JsonSerializer.Deserialize<EosFilterRequest>(request.Body.AsSpan()); }
        catch { return Results.BadRequest(EosError("common.bad_request", "Invalid JSON body", 1001)); }

        Logger.Information($"Matchmaking search | deployment={deployment_id} criteria={filterReq?.Criteria?.Count ?? 0} maxResults={filterReq?.MaxResults ?? 100}");

        List<EosSessionInfo> snapshot;
        lock (_sessionLock) { snapshot = Sessions.ToList(); }

        // Whitelisted stations are only advertised to the accounts on their list.
        var (callerName, callerPuid) = IdentifyCaller(request);
        int hidden = 0;
        if (_stationWhitelist.Count > 0)
        {
            int before = snapshot.Count;
            snapshot = snapshot.Where(s => VisibleTo(s, callerName, callerPuid)).ToList();
            hidden = before - snapshot.Count;
            if (hidden > 0)
                Logger.Information($"Whitelist | hid {hidden} session(s) from " +
                                   $"{(string.IsNullOrEmpty(callerName) ? "an unidentified caller" : callerName)}");
        }

        var matched = snapshot
            .Where(s => MatchesCriteria(s, filterReq?.Criteria))
            .Take(filterReq?.MaxResults > 0 ? filterReq.MaxResults : 100);

        var sessions = matched.Select(s => new EosMatchmakingSession
        {
            Deployment                   = DEPLOYMENT_ID,
            Id                           = s.Id,
            Bucket                       = s.Bucket,
            Settings = new EosSessionSettings
            {
                MaxPublicPlayers     = s.MaxPlayers,
                AllowInvites         = false,
                ShouldAdvertise      = true,
                AllowReadById        = true,
                AllowJoinViaPresence = true,
                AllowJoinInProgress  = true,
                AllowConferenceRoom  = false,
                CheckSanctions       = false,
                AllowMigration       = false,
                RejoinAfterKick      = "",
                Platforms            = null
            },
            MaxPublicPlayers             = s.MaxPlayers,
            OpenPublicPlayers            = s.MaxPlayers - s.PublicPlayers.Count,
            MaxPrivatePlayers            = 0,
            OpenPrivatePlayers           = 0,
            PublicPlayers                = s.PublicPlayers,
            PrivatePlayers               = new List<string>(),
            TotalPlayers                 = s.PublicPlayers.Count,
            AllowJoinInProgress          = true,
            ShouldAdvertise              = true,
            IsDedicated                  = true,
            UsesStats                    = false,
            AllowInvites                 = false,
            UsesPresence                 = false,
            AllowJoinViaPresence         = true,
            AllowJoinViaPresenceFriendsOnly = false,
            BuildUniqueId                = s.BuildId,
            Started                      = true,
            LastUpdated                  = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
            Attributes = new EosSessionAttributes
            {
                ServerName         = s.ServerName,
                BUsesStats         = false,
                NumPrivateConns    = 0,
                UsesPresence       = false,
                Address            = s.IpAddress,
                PresenceSearch     = true,
                NumPublicConns     = s.MaxPlayers,
                PublicPort         = s.Port,
                BuildUniqueId      = s.BuildId,
                AntiCheatProtected = true,
                IsDedicated        = true,
                ImguiPort          = s.ImguiPort,
            },
            Owner           = "Client_xyza7891d295O6HPItjLVgKsuU06DkR0",
            OwnerPlatformId = null
        }).ToList();

        var payload = new EosMatchmakingResponse { Count = sessions.Count, Sessions = sessions };
        var bytes   = JsonSerializer.SerializeToUtf8Bytes<EosMatchmakingResponse>(payload);
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", bytes);
    }

    // ── Lobbies ──────────────────────────────────────────────────────────────

    [HttpPost("/lobby/v1/{deployment_id}/lobbies/filter")]
    public async Task<IHttpActionResult> LobbyFilter(IHttpRequest request, IHttpResponse response, string deployment_id)
    {
        AddEosHeaders(response);

        EosLobbyFilterRequest? req = null;
        try { req = JsonSerializer.Deserialize<EosLobbyFilterRequest>(request.Body.AsSpan()); }
        catch { return Results.BadRequest(EosError("common.bad_request", "Invalid JSON body", 1001)); }

        Logger.Information($"Lobby filter | deployment={deployment_id} criteria={req?.Criteria?.Count ?? 0}");

        List<EosLobbySession> snapshot;
        lock (LobbyLock) { snapshot = Lobbies.ToList(); }

        int minPlayers = req?.MinCurrentPlayers ?? 0;
        int maxResults = req?.MaxResults > 0 ? req.MaxResults : 100;

        var matched = snapshot
            .Where(l => l.TotalPlayers >= minPlayers)
            .Where(l => MatchesLobbyCriteria(l, req?.Criteria))
            .Take(maxResults)
            .ToList();

        var payload = new EosLobbyResponse { Count = matched.Count, Sessions = matched };
        var bytes   = JsonSerializer.SerializeToUtf8Bytes(payload);
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", bytes);
    }

    [HttpPost("/lobby/v1/{deployment_id}/lobbies")]
    public async Task<IHttpActionResult> LobbyCreate(IHttpRequest request, IHttpResponse response, string deployment_id)
    {
        AddEosHeaders(response);

        EosLobbyCreateRequest? req = null;
        try { req = JsonSerializer.Deserialize<EosLobbyCreateRequest>(request.Body.AsSpan()); }
        catch { return Results.BadRequest(EosError("common.bad_request", "Invalid JSON body", 1001)); }

        request.Headers.TryGetValue("Authorization", out var auth);
        var ownerPuid = ExtractPuidFromBearer(auth);

        var lobby = new EosLobbySession
        {
            Deployment      = DEPLOYMENT_ID,
            Id              = Guid.NewGuid().ToString("N"),
            Bucket          = req?.Bucket ?? "main",
            Settings        = req?.Settings ?? new EosLobbySettings(),
            TotalPlayers    = 1,
            OpenPublicPlayers = (req?.Settings?.MaxPublicPlayers ?? 10) - 1,
            PublicPlayers   = new List<string> { ownerPuid },
            Started         = false,
            LastUpdated     = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
            Attributes      = req?.Attributes ?? new Dictionary<string, string?>(),
            Owner           = ownerPuid,
            OwnerPlatformId = 4000,
        };

        lock (LobbyLock) { Lobbies.Add(lobby); }
        Logger.Information($"Lobby created | id={lobby.Id} owner={ownerPuid}");
        PersistLobbies();

        var bytes = JsonSerializer.SerializeToUtf8Bytes(LobbyInfoEnvelope(lobby));
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", bytes);
    }

    [HttpGet("/lobby/v1/{deployment_id}/lobbies/{lobby_id}")]
    public async Task<IHttpActionResult> LobbyGet(IHttpRequest request, IHttpResponse response, string deployment_id, string lobby_id)
    {
        AddEosHeaders(response);

        EosLobbySession? lobby;
        lock (LobbyLock) { lobby = Lobbies.FirstOrDefault(l => l.Id == lobby_id); }

        if (lobby == null) return Results.NotFound(EosError("lobbies.not_found", $"Lobby {lobby_id} not found", 10069));

        var bytes = JsonSerializer.SerializeToUtf8Bytes(LobbyInfoEnvelope(lobby));
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", bytes);
    }

    [HttpPut("/lobby/v1/{deployment_id}/lobbies/{lobby_id}")]
    public async Task<IHttpActionResult> LobbyUpdate(IHttpRequest request, IHttpResponse response, string deployment_id, string lobby_id)
    {
        AddEosHeaders(response);

        EosLobbyUpdateRequest? req = null;
        try { req = JsonSerializer.Deserialize<EosLobbyUpdateRequest>(request.Body.AsSpan()); }
        catch { return Results.BadRequest(EosError("common.bad_request", "Invalid JSON body", 1001)); }

        lock (LobbyLock)
        {
            var lobby = Lobbies.FirstOrDefault(l => l.Id == lobby_id);
            if (lobby == null) return Results.NotFound(EosError("lobbies.not_found", $"Lobby {lobby_id} not found", 10069));

            if (req?.Settings != null)   lobby.Settings = req.Settings;
            if (req?.Attributes != null) foreach (var kv in req.Attributes) lobby.Attributes[kv.Key] = kv.Value;
            if (req?.Started != null)    lobby.Started = req.Started.Value;
            lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
            lobby.OpenPublicPlayers = lobby.Settings.MaxPublicPlayers - lobby.PublicPlayers.Count;

            var bytes = JsonSerializer.SerializeToUtf8Bytes(LobbyInfoEnvelope(lobby));
            PersistLobbies();
            return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", bytes);
        }
    }

    [HttpPost("/lobby/v1/{deployment_id}/lobbies/{lobby_id}/members/{product_user_id}")]
    public async Task<IHttpActionResult> LobbyJoin(IHttpRequest request, IHttpResponse response, string deployment_id, string lobby_id, string product_user_id)
    {
        AddEosHeaders(response);

        lock (LobbyLock)
        {
            var lobby = Lobbies.FirstOrDefault(l => l.Id == lobby_id);
            if (lobby == null) return Results.NotFound(EosError("lobbies.not_found", $"Lobby {lobby_id} not found", 10069));

            if (!lobby.PublicPlayers.Contains(product_user_id))
            {
                lobby.PublicPlayers.Add(product_user_id);
                lobby.TotalPlayers    = lobby.PublicPlayers.Count;
                lobby.OpenPublicPlayers = Math.Max(0, lobby.Settings.MaxPublicPlayers - lobby.TotalPlayers);
                lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
            }
        }

        Logger.Information($"Lobby join | id={lobby_id} player={product_user_id}");
        _ = LobbyWsHelper.BroadcastAsync(lobby_id, new
        {
            name    = "memberjoined",
            payload = new { lobbyId = lobby_id, puid = product_user_id, platformId = 4000, platform = "steam", allowCrossplay = true, name = "memberjoined" }
        });
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json");
    }

    [HttpDelete("/lobby/v1/{deployment_id}/lobbies/{lobby_id}/members/{product_user_id}")]
    public async Task<IHttpActionResult> LobbyLeave(IHttpRequest request, IHttpResponse response, string deployment_id, string lobby_id, string product_user_id)
    {
        AddEosHeaders(response);

        lock (LobbyLock)
        {
            var lobby = Lobbies.FirstOrDefault(l => l.Id == lobby_id);
            if (lobby != null)
            {
                lobby.PublicPlayers.Remove(product_user_id);
                lobby.TotalPlayers      = lobby.PublicPlayers.Count;
                lobby.OpenPublicPlayers = Math.Max(0, lobby.Settings.MaxPublicPlayers - lobby.TotalPlayers);
                lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");

                // destroy lobby if empty or owner left
                if (lobby.TotalPlayers == 0 || lobby.Owner == product_user_id)
                    Lobbies.Remove(lobby);
            }
        }

        Logger.Information($"Lobby leave | id={lobby_id} player={product_user_id}");
        _ = LobbyWsHelper.BroadcastAsync(lobby_id, new
        {
            name    = "memberleft",
            payload = new { lobbyId = lobby_id, puid = product_user_id, name = "memberleft" }
        });
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json");
    }

    private static object LobbyInfoEnvelope(EosLobbySession lobby) => new
    {
        publicData  = (object)lobby,
        privateData = (object?)null,
        memberData  = (object?)null,
        room        = (object?)null,
        requestId   = (object?)null,
        name        = "lobbyinfo",
    };

    private static object EosError(string code, string message, int numericCode = 0) => new
    {
        errorCode        = $"errors.com.epicgames.{code}",
        errorMessage     = message,
        numericErrorCode = numericCode,
        messageVarList   = Array.Empty<string>(),
        correlationId    = CORRELATION_ID,
    };

    private static bool MatchesLobbyCriteria(EosLobbySession lobby, List<EosFilterCriterion>? criteria)
    {
        if (criteria == null || criteria.Count == 0) return true;
        foreach (var c in criteria)
        {
            // criteria keys look like "attributes.VERSION_s" — strip the prefix
            var attrKey = c.Key.StartsWith("attributes.", StringComparison.OrdinalIgnoreCase)
                ? c.Key[11..]
                : c.Key;

            if (!lobby.Attributes.TryGetValue(attrKey, out var attrVal)) return false;

            bool match = c.Op switch
            {
                "EQUAL"     => attrVal == c.Value.GetString(),
                "NOT_EQUAL" => attrVal != c.Value.GetString(),
                _ => true
            };
            if (!match) return false;
        }
        return true;
    }

    internal static string ExtractPuidFromBearer(string? authHeader)
    {
        if (string.IsNullOrEmpty(authHeader) || !authHeader.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase))
            return "0002000000000000000000000000000000";
        try
        {
            var parts = authHeader[7..].Split('.');
            if (parts.Length < 2) return "0002000000000000000000000000000000";
            var pad     = parts[1].Length % 4;
            var padded  = pad == 0 ? parts[1] : parts[1] + new string('=', 4 - pad);
            var json    = System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(padded.Replace('-', '+').Replace('_', '/')));
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.TryGetProperty("productUserId", out var v) ? v.GetString() ?? "" : "";
        }
        catch { return "0002000000000000000000000000000000"; }
    }

    private static string SteamIdFromTicket(string? ticketHex)
    {
        if (string.IsNullOrEmpty(ticketHex)) return "76561190000000001";
        var hash = System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.ASCII.GetBytes(ticketHex));
        var num  = BitConverter.ToUInt64(hash, 0) & 0x000FFFFFFFFFFFFF; // 52-bit
        return $"7656119{num:D10}";
    }

    private static bool MatchesCriteria(EosSessionInfo s, List<EosFilterCriterion>? criteria)
    {
        if (criteria == null || criteria.Count == 0) return true;
        foreach (var c in criteria)
        {
            var val = ResolveKey(s, c.Key);
            if (!EvalOp(val, c.Op, c.Value)) return false;
        }
        return true;
    }

    private static object? ResolveKey(EosSessionInfo s, string key) => key switch
    {
        "bucket"                                => (object)s.Bucket,
        "attributes.SERVERNAME_s"               => s.ServerName,
        "attributes.ADDRESS_s"                  => s.IpAddress,
        "attributes.PUBLICPORT_s"               => s.Port,
        "attributes.NUMPUBLICCONNECTIONS_l"     => s.MaxPlayers,
        "attributes.BUILDUNIQUEID_l"            => s.BuildId,
        "attributes.IMGUIPORT_s"               => s.ImguiPort,
        "attributes.BISDEDICATED_b"             => true,
        "attributes.BANTICHEATPROTECTED_b"      => true,
        "attributes.PRESENCESEARCH_b"           => true,
        "attributes.BUSESSTATS_b"               => false,
        "attributes.USESPRESENCE_b"             => false,
        "attributes.NUMPRIVATECONNECTIONS_l"    => 0,
        _ => null
    };

    private static bool EvalOp(object? val, string op, JsonElement crit)
    {
        try
        {
            return op switch
            {
                "EQUAL" => val switch
                {
                    string sv => sv == crit.GetString(),
                    int    iv => crit.TryGetInt32(out var ci) && iv == ci,
                    bool   bv => (crit.ValueKind == JsonValueKind.True && bv) || (crit.ValueKind == JsonValueKind.False && !bv),
                    _ => false
                },
                "NOT_EQUAL" => val switch
                {
                    string sv => sv != crit.GetString(),
                    int    iv => !crit.TryGetInt32(out var ci) || iv != ci,
                    _ => false
                },
                "GREATER_THAN_OR_EQUAL" => val is int ge && crit.TryGetInt32(out var gei) && ge >= gei,
                "GREATER_THAN"          => val is int gt && crit.TryGetInt32(out var gti) && gt >  gti,
                "LESS_THAN_OR_EQUAL"    => val is int le && crit.TryGetInt32(out var lei) && le <= lei,
                "LESS_THAN"             => val is int lt && crit.TryGetInt32(out var lti) && lt <  lti,
                _ => true
            };
        }
        catch { return false; }
    }

    [HttpPost("/matchmaking/v1/{deployment_id}/sessions/{session_id}/publicplayers/{product_user_id}")]
    public async Task<IHttpActionResult> JoinSession(IHttpRequest request, IHttpResponse response, string deployment_id, string session_id, string product_user_id)
    {
        AddEosHeaders(response);

        lock (_sessionLock)
        {
            var session = Sessions.FirstOrDefault(s => s.Id == session_id);
            if (session != null && !session.PublicPlayers.Contains(product_user_id))
                session.PublicPlayers.Add(product_user_id);
        }

        Logger.Information($"Player joined session | session={session_id} player={product_user_id}");
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json");
    }

    [HttpDelete("/matchmaking/v1/{deployment_id}/sessions/{session_id}/publicplayers/{product_user_id}")]
    public async Task<IHttpActionResult> LeaveSession(IHttpRequest request, IHttpResponse response, string deployment_id, string session_id, string product_user_id)
    {
        AddEosHeaders(response);

        lock (_sessionLock)
        {
            var session = Sessions.FirstOrDefault(s => s.Id == session_id);
            session?.PublicPlayers.Remove(product_user_id);
        }

        Logger.Information($"Player left session | session={session_id} player={product_user_id}");
        return Results.Configurable(System.Net.HttpStatusCode.OK, "application/json");
    }

    // ── User search ──────────────────────────────────────────────────────────

    [HttpPost("/user/v3/product-users/search")]
    public async Task<IHttpActionResult> ProductUserSearch(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);

        var bodyStr = request.Body is byte[] b ? Encoding.UTF8.GetString(b) : request.Body?.ToString() ?? "";

        var result = new Dictionary<string, object>();
        try
        {
            using var doc = JsonDocument.Parse(bodyStr);
            if (doc.RootElement.TryGetProperty("productUserIds", out var ids))
            {
                foreach (var el in ids.EnumerateArray())
                {
                    var puid = el.GetString();
                    if (string.IsNullOrEmpty(puid)) continue;
                    var displayName = _userNames.TryGetValue(puid, out var n) ? n : puid[..8];
                    result[puid] = new
                    {
                        accounts = new[]
                        {
                            new
                            {
                                accountId          = puid,
                                displayName,
                                identityProviderId = "deviceid",
                                lastLogin          = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
                            }
                        }
                    };
                }
            }
        }
        catch { }

        Logger.Information($"ProductUser search | resolved {result.Count} puids");
        return Results.Ok(new { productUsers = result });
    }

    // ── Telemetry / DataRouter ────────────────────────────────────────────────

    [HttpPost("/telemetry/data/datarouter/api/v1/public/data")]
    public async Task<IHttpActionResult> Telemetry(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);
        var raw = Encoding.UTF8.GetString(request.Body);
        EosObservabilityStore.PushTelemetry("telemetry.data", raw);
        return Results.Ok(new { });
    }

    [HttpPost("/datarouter/api/v1/public/data/clients")]
    public async Task<IHttpActionResult> DataRouterEvent(IHttpRequest request, IHttpResponse response)
    {
        AddEosHeaders(response);
        try
        {
            var payload = JsonSerializer.Deserialize<EosTelemetryRequest>(request.Body.AsSpan());
            if (payload?.Events != null)
                foreach (var ev in payload.Events)
                    EosObservabilityStore.PushTelemetry(ev.EventName, JsonSerializer.Serialize(ev));
        }
        catch { }
        return Results.Ok(new { });
    }
}
