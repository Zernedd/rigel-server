using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.EOSSDK;
using AUnrealFeatures.EOSSDK.Models;
using AUnrealFeatures.HalcyonSocket;
using System.Reflection;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Servers;

public interface IAresDashboardServer { }

public sealed class AresDashboardServer : AstraHttpServer, IAresDashboardServer
{
    const string HOSTNAME = "localhost";
    const ushort PORT = 8080;

    static readonly string DashboardHtml;

    static AresDashboardServer()
    {
        var asm = Assembly.GetExecutingAssembly();
        var name = asm.GetManifestResourceNames()
            .FirstOrDefault(n => n.EndsWith("index.html", StringComparison.OrdinalIgnoreCase));
        if (name != null)
        {
            using var stream = asm.GetManifestResourceStream(name)!;
            using var reader = new StreamReader(stream);
            DashboardHtml = reader.ReadToEnd();
        }
        else
        {
            DashboardHtml = "<h1>Dashboard HTML not found</h1>";
        }
    }

    public AresDashboardServer() : base(HOSTNAME, PORT)
    {
        // Gate /api/* on this server only (dashboard). Capture-first (log-only) until SSO sessions land.
        AddPreprocessor<DashboardAuthPreprocessor>();
    }

    private static string GenerateDashboardApiKey(string ownerId, string keyType = "service")
    {
        var keyId = Guid.NewGuid().ToString();
        var createdAt = DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss.ffffff");
        var payload = new { key_id = keyId, key_type = keyType, owner_id = ownerId, created_at = createdAt };
        var header = Base64UrlEncode(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new { alg = "HS256", typ = "JWT" }));
        var body = Base64UrlEncode(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(payload));
        var signing = $"{header}.{body}";
        using var hmac = new System.Security.Cryptography.HMACSHA256(System.Text.Encoding.UTF8.GetBytes("astra-api-key-secret"));
        return $"{signing}.{Base64UrlEncode(hmac.ComputeHash(System.Text.Encoding.UTF8.GetBytes(signing)))}";
    }

    private static string Base64UrlEncode(byte[] data)
        => Convert.ToBase64String(data).Replace("+", "-").Replace("/", "_").TrimEnd('=');


    // ── UI ───────────────────────────────────────────────────────────────────

    [HttpGet("/")]
    public async Task<IHttpActionResult> Index(IHttpRequest request, IHttpResponse response)
    {
        return Results.HtmlDocument(System.Net.HttpStatusCode.OK, DashboardHtml);
    }

    // ── Meta (Oculus) SSO ──────────────────────────────────────────────────────
    // Mirrors the real A2/Orion flow: browser -> https://auth.oculus.com/sso/?redirect_uri=<dash>/auth/meta
    // &organization_id=<org>; Meta authenticates and redirects back to /auth/meta with a token/code we
    // validate server-side, then we issue OUR signed session cookie and gate the API on it.
    //   PREREQS (Meta org settings): redirect_uri below MUST be registered exactly, and org id is ours.
    //   NOTE: if a Next.js proxy fronts the dashboard, add an /auth/* -> :8080 rewrite so these are reachable.
    const string META_ORG_ID      = "1458804674917108";     // OUR Meta organization
    const string OCULUS_SSO_URL    = "https://auth.oculus.com/sso/";
    const string DASHBOARD_ORIGIN  = "http://localhost:3000";   // dev origin for now (must be registered as a redirect_uri in the Meta org; swap to the real domain for prod)

    [HttpGet("/auth/login")]
    public async Task<IHttpActionResult> AuthLogin(IHttpRequest request, IHttpResponse response)
    {
        var redirectUri = DASHBOARD_ORIGIN + "/auth/meta";
        var sso = OCULUS_SSO_URL + "?redirect_uri=" + Uri.EscapeDataString(redirectUri)
                + "&organization_id=" + META_ORG_ID;
        // JS + meta-refresh redirect (framework-agnostic) so the browser bounces to Meta sign-in.
        return Results.HtmlDocument(System.Net.HttpStatusCode.OK,
            $"<!doctype html><meta http-equiv='refresh' content='0;url={sso}'>" +
            $"<script>location.href={System.Text.Json.JsonSerializer.Serialize(sso)}</script>Redirecting to Meta sign-in…");
    }

    // CAPTURE PHASE — log EXACTLY what Meta returns so we can write the validator. Once we know the
    // callback param (a code/token) + how to verify it against graph.oculus.com, this becomes:
    //   validate token -> get Meta user id -> require org membership + admin allowlist ->
    //   response.SetCookie(signed session JWT) -> response.Redirect("/").  Until then it just captures.
    [HttpGet("/auth/meta")]
    public async Task<IHttpActionResult> AuthMetaCallback(IHttpRequest request, IHttpResponse response)
    {
        var q = string.Join(" | ", request.Queries.Select(kv => $"{kv.Key}={kv.Value}"));
        Console.WriteLine($"[DASH-SSO] /auth/meta callback remote={request.Remote} host={request.Host} " +
                          $"uri={request.Uri} queries=[{q}] referer={request.GetHeaderValue("Referer")}");
        return Results.HtmlDocument(System.Net.HttpStatusCode.OK,
            "<h2>Meta sign-in received</h2><p>Callback captured — check the server log line tagged " +
            "<code>[DASH-SSO]</code> for the params, then we finish the validator + session.</p>");
    }

    // ── API ──────────────────────────────────────────────────────────────────

    [HttpGet("/api/stats")]
    public async Task<IHttpActionResult> GetStats(IHttpRequest request, IHttpResponse response)
    {
        var users       = Program.Database.GetCollection<UserDataResponse>(true);
        var stations    = Program.Database.GetCollection<StationDbObject>(true);
        var deployments = Program.Database.GetCollection<DeploymentDbObject>(true);
        var events      = Program.Database.GetCollection<ServerEventDbObject>(true);

        var allDeployments = deployments?.FindAll().ToList() ?? new();

        return Results.Ok(new DashboardStats
        {
            TotalUsers       = users?.FindAll().Count() ?? 0,
            TotalStations    = stations?.FindAll().Count() ?? 0,
            TotalDeployments = allDeployments.Count,
            OnlineDeployments = allDeployments.Count(d => d.Online),
            TotalPlayers     = allDeployments.Where(d => d.Online).Sum(d => d.PlayerCount),
            TotalEvents = events?.FindAll().Count() ?? 0
        });
    }

    private const string GlobalAdminRoleId = "__global_admin__";

    private static RoleResponse EnsureGlobalAdminRole()
    {
        var roles = Program.Database.GetCollection<RoleResponse>(true);
        var existing = roles?.FindOne(r => r.RoleId == GlobalAdminRoleId);
        if (existing != null) return existing;
        var role = new RoleResponse
        {
            RoleId = GlobalAdminRoleId,
            StationId = "global",
            RoleName = "Global Administrator",
            RoleDescription = "Full access to all permissions",
            Permissions = new List<string> { "global:admin" }
        };
        roles?.Insert(role);
        return role;
    }

    [HttpGet("/api/users")]
    public async Task<IHttpActionResult> GetUsers(IHttpRequest request, IHttpResponse response)
    {
        var users = Program.Database.GetCollection<UserDataResponse>(true);
        // Skip records with no UserId. The login paths can leave a half-written user behind (no id,
        // no username) and such a row is not just cosmetic noise -- every action on this tab is keyed
        // by user_id, so it is unactionable, and the UI renders user_id.slice(0,8), which threw on the
        // null and took the WHOLE users table down with it. Filter at the source, and never hand the
        // client a null where it expects a string.
        var list = users?.FindAll()
            .Where(u => !string.IsNullOrWhiteSpace(u.UserId))
            .OrderByDescending(u => u.LastLogin)
            .Take(50)
            .Select(u => new DashboardUser
            {
                UserId    = u.UserId ?? string.Empty,
                Username  = string.IsNullOrWhiteSpace(u.Username) ? "(unknown)" : u.Username,
                Platform  = u.Platform,
                LastLogin = u.LastLogin,
                CreatedAt = u.CreatedAt,
                RoleCount = u.Roles?.Count ?? 0,
                Banned    = u.Bans?.Any(b => !b.Revoked && (b.Expiration == null || b.Expiration > DateTime.UtcNow)) ?? false,
                IsAdmin   = u.Roles?.Contains(GlobalAdminRoleId) ?? false
            })
            .ToList() ?? new();

        return Results.Ok(list);
    }

    [HttpPost("/api/users/{user_id}/grant_admin")]
    public async Task<IHttpActionResult> GrantAdmin(IHttpRequest request, IHttpResponse response, string user_id)
    {
        var userCol = Program.Database.GetCollection<UserDataResponse>(true);
        var user = userCol?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        EnsureGlobalAdminRole();
        user.Roles ??= new List<string>();
        if (!user.Roles.Contains(GlobalAdminRoleId))
        {
            user.Roles.Add(GlobalAdminRoleId);
            userCol?.Update(user);
        }
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    [HttpDelete("/api/users/{user_id}/grant_admin")]
    public async Task<IHttpActionResult> RevokeAdmin(IHttpRequest request, IHttpResponse response, string user_id)
    {
        var userCol = Program.Database.GetCollection<UserDataResponse>(true);
        var user = userCol?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        if (user.Roles?.Remove(GlobalAdminRoleId) == true)
            userCol?.Update(user);

        return Results.Ok(new DashboardActionResult { Success = true });
    }

    // --- BOARD IMAGE UPLOAD (dashboard side) --------------------------------------
    // The dashboard page is served on :8080 while the board images must be SERVED from the
    // publicly reachable StationDb origin on :78 (that is what the Quest client fetches). Both
    // servers run in the SAME process against the same LiteDB and the same boards/ folder, so the
    // upload is handled here -- no cross-origin request from the browser, and no API key has to be
    // embedded in the page -- while A2StationDbServer keeps GET /board/{file} for public serving.
    //
    //   POST /api/stations/{station_id}/board?key=<BoardTextureUrl1|SignPlazaFront|...>
    //   body: raw image bytes (png/jpg/gif/webp)
    //
    // The stored URL must be the EXTERNAL origin, since the headset resolves it, not this browser.
    // BOARD_PUBLIC_BASE sets it; it falls back to the known public StationDb hostname.
    [HttpPost("/api/stations/{station_id}/board")]
    public Task<IHttpActionResult> UploadStationBoard(IHttpRequest request, IHttpResponse response, string station_id)
    {
        string key = request.GetQueryParameter("key", "") ?? "";
        if (Array.IndexOf(A2StationDbServer.BoardConfigKeys, key) < 0)
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "unknown board key", allowed = A2StationDbServer.BoardConfigKeys }));

        byte[] body = request.Body ?? Array.Empty<byte>();
        if (body.Length == 0)
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "empty body" }));
        if (body.Length > 8 * 1024 * 1024)
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "image too large (max 8MB)" }));

        string ext = A2StationDbServer.SniffImageExtensionPublic(body);
        if (ext == null)
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "not a png/jpg/gif/webp image" }));

        var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
        var station = stationCollection?.FindOne(s => s.StationId == station_id);
        if (station == null)
            return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));

        string dir = A2StationDbServer.BoardUploadDirPublic;
        Directory.CreateDirectory(dir);
        string safeStation = A2StationDbServer.SanitiseForFileNamePublic(station_id);
        string fileName = $"{safeStation}_{key}_{DateTimeOffset.UtcNow.ToUnixTimeSeconds()}.{ext}";

        try
        {
            foreach (var stale in Directory.GetFiles(dir, $"{safeStation}_{key}_*"))
                File.Delete(stale);
        }
        catch { /* a leftover file is harmless; never fail the upload over cleanup */ }

        File.WriteAllBytes(Path.Combine(dir, fileName), body);

        string baseUrl = (Environment.GetEnvironmentVariable("BOARD_PUBLIC_BASE") ?? "").TrimEnd('/');
        if (string.IsNullOrWhiteSpace(baseUrl))
            baseUrl = "https://rigel.wwiggles.org";
        string url = $"{baseUrl}/board/{fileName}";

        station.Config[key] = url;
        stationCollection?.Update(station);

        return Task.FromResult<IHttpActionResult>(Results.Ok(new { success = true, key, url, bytes = body.Length }));
    }

    // GET /api/board/keys -- the uploadable slots, so the page can render one row per board.
    [HttpGet("/api/board/keys")]
    public Task<IHttpActionResult> BoardKeys(IHttpRequest request, IHttpResponse response)
        => Task.FromResult<IHttpActionResult>(Results.Ok(A2StationDbServer.BoardConfigKeys));

    // GET /api/stations/{station_id}/board -- current board URLs for this station.
    [HttpGet("/api/stations/{station_id}/board")]
    public Task<IHttpActionResult> GetStationBoard(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
        var station = stationCollection?.FindOne(s => s.StationId == station_id);
        var result = new Dictionary<string, string>();
        foreach (var k in A2StationDbServer.BoardConfigKeys)
            result[k] = (station != null && station.Config.TryGetValue(k, out var v)) ? v : "";
        return Task.FromResult<IHttpActionResult>(Results.Ok(result));
    }

    // DELETE /api/stations/{station_id}/board?key=... -- clear one board back to blank.
    [HttpDelete("/api/stations/{station_id}/board")]
    public Task<IHttpActionResult> ClearStationBoard(IHttpRequest request, IHttpResponse response, string station_id)
    {
        string key = request.GetQueryParameter("key", "") ?? "";
        var stationCollection = Program.Database.GetCollection<StationDbObject>(true);
        var station = stationCollection?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound());
        station.Config.Remove(key);
        stationCollection?.Update(station);
        return Task.FromResult<IHttpActionResult>(Results.Ok(new { success = true, key }));
    }

    // --- NETVARS -----------------------------------------------------------------------------------
    // The game server reports its entire netvar tree as a "netvars" server_event; NetvarDumpStore keeps
    // the latest one per deployment. These routes expose it to the dashboard, plus direct editing of a
    // station's config key/values -- the flat map the game pulls on its deployment fetch
    // (GET /v1/deployments/{id}?include_station_config=true) and applies natively as netvars.

    // GET /api/netvars/dumps -- which deployments have reported a dump, how big, how fresh.
    [HttpGet("/api/netvars/dumps")]
    public Task<IHttpActionResult> ListNetvarDumps(IHttpRequest request, IHttpResponse response)
        => Task.FromResult<IHttpActionResult>(Results.Ok(NetvarDumpStore.List()));

    // GET /api/netvars/dump?deployment_id=<id>&type=netvars|state -- the raw JSON the game sent.
    // Omit deployment_id for the most recent one.
    [HttpGet("/api/netvars/dump")]
    public Task<IHttpActionResult> GetNetvarDump(IHttpRequest request, IHttpResponse response)
    {
        string? dep = request.GetQueryParameter("deployment_id", "");
        string type = request.GetQueryParameter("type", "netvars") ?? "netvars";
        var d = NetvarDumpStore.Get(dep, type);
        if (d == null)
            return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no dump received yet", deployment_id = dep, type }));
        return Task.FromResult<IHttpActionResult>(
            Results.Configurable(System.Net.HttpStatusCode.OK, "application/json", System.Text.Encoding.UTF8.GetBytes(d.Data)));
    }

    // GET /api/stations/{station_id}/config -- the station's own key/values (not the served defaults).
    [HttpGet("/api/stations/{station_id}/config")]
    public Task<IHttpActionResult> GetStationConfig(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var station = Program.Database.GetCollection<StationDbObject>(true)?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));
        return Task.FromResult<IHttpActionResult>(Results.Ok(station.Config ?? new Dictionary<string, string>()));
    }

    // PATCH /api/stations/{station_id}/config -- merge {key: value} pairs into the station config.
    [HttpPatch("/api/stations/{station_id}/config")]
    public Task<IHttpActionResult> PatchStationConfig(IHttpRequest request, IHttpResponse response, string station_id)
    {
        Dictionary<string, string>? data;
        try { data = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, string>>(request.Body); }
        catch { data = null; }
        if (data == null || data.Count == 0)
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "body must be a JSON object of string keys to string values" }));

        var stations = Program.Database.GetCollection<StationDbObject>(true);
        var station = stations?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));
        station.Config ??= new Dictionary<string, string>();
        foreach (var kv in data)
        {
            if (string.IsNullOrWhiteSpace(kv.Key)) continue;
            station.Config[kv.Key.Trim()] = kv.Value ?? "";
        }
        stations!.Update(station);
        NetvarOverridesFile.OnStationConfigChanged(station_id);   // push to co-located game servers
        StationAcl.Push(station_id);                              // push the station browser whitelist
        return Task.FromResult<IHttpActionResult>(Results.Ok(station.Config));
    }

    // ── SPEC EDITOR SAVED LEVELS ─────────────────────────────────────────────────────────────
    // Levels are uploaded by game servers over port 78 (v1/spec/*); see SpecLevelsStore. The dashboard only
    // views them and flips autoload / loaded (loaded = desired state; servers poll it and apply live).

    // GET /api/spec/levels
    [HttpGet("/api/spec/levels")]
    public Task<IHttpActionResult> GetSpecLevels(IHttpRequest request, IHttpResponse response)
    {
        var list = SpecLevelsStore.List().Select(l => new
        {
            name         = l.Name,
            autoload     = l.Entry.Autoload,
            loaded       = l.Entry.Loaded,
            updated      = l.Entry.Updated,
            size         = l.Entry.Size,
            serverLoaded = l.Entry.ServerLoaded,
            serverSeen   = l.Entry.ServerSeen,
        }).ToList();
        return Task.FromResult<IHttpActionResult>(Results.Ok(list));
    }

    // GET /api/spec/levels/{name}
    [HttpGet("/api/spec/levels/{name}")]
    public Task<IHttpActionResult> GetSpecLevel(IHttpRequest request, IHttpResponse response, string name)
    {
        var n = SpecLevelsStore.CleanName(name);
        if (n == null) return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "bad level name" }));
        var text = SpecLevelsStore.ReadText(n);
        if (text == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such level", name = n }));
        return Task.FromResult<IHttpActionResult>(Results.Ok(new { name = n, text }));
    }

    // PATCH /api/spec/levels/{name}  body: { autoload?: bool, loaded?: bool }
    [HttpPatch("/api/spec/levels/{name}")]
    public Task<IHttpActionResult> PatchSpecLevel(IHttpRequest request, IHttpResponse response, string name)
    {
        var n = SpecLevelsStore.CleanName(name);
        if (n == null) return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "bad level name" }));
        SpecLevelPatchBody? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<SpecLevelPatchBody>(request.Body); }
        catch { body = null; }
        if (body == null || (body.Autoload == null && body.Loaded == null))
            return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "body must be { autoload?: bool, loaded?: bool }" }));
        if (!SpecLevelsStore.Update(n, body.Autoload, body.Loaded))
            return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such level", name = n }));
        return Task.FromResult<IHttpActionResult>(Results.Ok(new { ok = true }));
    }

    // DELETE /api/spec/levels/{name}
    [HttpDelete("/api/spec/levels/{name}")]
    public Task<IHttpActionResult> DeleteSpecLevel(IHttpRequest request, IHttpResponse response, string name)
    {
        var n = SpecLevelsStore.CleanName(name);
        if (n == null) return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "bad level name" }));
        if (!SpecLevelsStore.Delete(n))
            return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such level", name = n }));
        return Task.FromResult<IHttpActionResult>(Results.Ok(new { ok = true }));
    }

    private sealed class SpecLevelPatchBody
    {
        [JsonPropertyName("autoload")] public bool? Autoload { get; set; }
        [JsonPropertyName("loaded")]   public bool? Loaded { get; set; }
    }

    // ── STATION WHITELIST ────────────────────────────────────────────────────────────────────
    // The whitelist already lives in the station Config under "acl.whitelist" as a comma-separated
    // list of usernames, and PatchStationConfig can already write it. These two endpoints exist so the
    // dashboard can treat it as what it actually is -- a LIST OF ACCOUNTS -- instead of making someone
    // hand-edit a comma-separated string in a generic config box, where one stray comma silently
    // changes who can see the station.
    //
    // Returned alongside the list: every known Oculus account, so the UI can offer a picker rather than
    // relying on somebody typing a username exactly right. A username that does not match an account is
    // kept (it may be someone who has not logged in yet) but flagged so the UI can warn.

    // GET /api/stations/{station_id}/whitelist
    [HttpGet("/api/stations/{station_id}/whitelist")]
    public Task<IHttpActionResult> GetStationWhitelist(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var stations = Program.Database.GetCollection<StationDbObject>(true);
        var station = stations?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));

        string? raw = null;
        station.Config?.TryGetValue(StationAcl.WhitelistKey, out raw);
        var listed = StationAcl.Parse(raw).ToList();

        // Every account, not the 50 most recent that /api/users returns -- a picker that silently omits
        // the person you are looking for is worse than no picker. Same guard as GetUsers: half-written
        // login rows with no id or no username are unactionable here too.
        var users = Program.Database.GetCollection<UserDataResponse>(true)?.FindAll()
                        .Where(u => !string.IsNullOrWhiteSpace(u.UserId) && !string.IsNullOrWhiteSpace(u.Username))
                        .Select(u => new
                        {
                            user_id    = u.UserId!,
                            username   = u.Username!,
                            platform   = u.Platform,
                            last_login = u.LastLogin,
                        })
                        .OrderBy(u => u.username, StringComparer.OrdinalIgnoreCase)
                        .ToList()
                    ?? new();

        var known = new HashSet<string>(users.Select(u => u.username), StringComparer.OrdinalIgnoreCase);
        return Task.FromResult<IHttpActionResult>(Results.Ok(new
        {
            station_id,
            // An empty list is not "nobody" -- it is the default, "visible to everyone". The UI must
            // say so plainly, because an allowlist that reads as empty-means-locked is how you
            // accidentally hide a station from every player.
            enabled  = listed.Count > 0,
            entries  = listed.Select(n => new { username = n, known = known.Contains(n) }).ToList(),
            accounts = users,
        }));
    }

    // PUT /api/stations/{station_id}/whitelist  body: { "usernames": ["a","b"] }
    [HttpPut("/api/stations/{station_id}/whitelist")]
    public Task<IHttpActionResult> PutStationWhitelist(IHttpRequest request, IHttpResponse response, string station_id)
    {
        WhitelistBody? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<WhitelistBody>(request.Body); }
        catch { body = null; }
        if (body == null) return Task.FromResult<IHttpActionResult>(Results.BadRequest(new { error = "body must be {\"usernames\": [..]}" }));

        var stations = Program.Database.GetCollection<StationDbObject>(true);
        var station = stations?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));

        // Normalise here rather than trusting the caller: trim, drop blanks, de-duplicate
        // case-insensitively. A duplicate is harmless but makes the stored value confusing to read.
        var clean = new List<string>();
        foreach (var n in body.Usernames ?? new List<string>())
        {
            var t = (n ?? "").Trim();
            if (t.Length == 0 || t.Contains(',')) continue;          // a comma would split into two entries
            if (!clean.Any(e => string.Equals(e, t, StringComparison.OrdinalIgnoreCase))) clean.Add(t);
        }

        station.Config ??= new Dictionary<string, string>();
        if (clean.Count == 0) station.Config.Remove(StationAcl.WhitelistKey);   // back to visible-to-everyone
        else station.Config[StationAcl.WhitelistKey] = string.Join(", ", clean);
        stations!.Update(station);

        NetvarOverridesFile.OnStationConfigChanged(station_id);
        StationAcl.Push(station_id);            // takes effect immediately; no restart, no redeploy
        Console.WriteLine($"[ACL] {station_id} whitelist set to {clean.Count} account(s)");

        return Task.FromResult<IHttpActionResult>(Results.Ok(new
        {
            success = true, station_id, enabled = clean.Count > 0, usernames = clean,
        }));
    }

    private sealed class WhitelistBody
    {
        [System.Text.Json.Serialization.JsonPropertyName("usernames")]
        public List<string>? Usernames { get; set; }
    }

    // DELETE /api/stations/{station_id}/config?key=<name> -- remove one key (falls back to the default).
    [HttpDelete("/api/stations/{station_id}/config")]
    public Task<IHttpActionResult> DeleteStationConfigKey(IHttpRequest request, IHttpResponse response, string station_id)
    {
        string key = request.GetQueryParameter("key", "") ?? "";
        var stations = Program.Database.GetCollection<StationDbObject>(true);
        var station = stations?.FindOne(s => s.StationId == station_id);
        if (station == null) return Task.FromResult<IHttpActionResult>(Results.NotFound(new { error = "no such station", station_id }));
        bool removed = station.Config?.Remove(key) ?? false;
        if (removed) { stations!.Update(station); NetvarOverridesFile.OnStationConfigChanged(station_id); StationAcl.Push(station_id); }
        return Task.FromResult<IHttpActionResult>(Results.Ok(new { success = removed, key }));
    }

    [HttpGet("/api/stations")]
    public async Task<IHttpActionResult> GetStations(IHttpRequest request, IHttpResponse response)
    {
        var stations    = Program.Database.GetCollection<StationDbObject>(true);
        var deployments = Program.Database.GetCollection<DeploymentDbObject>(true);

        var list = stations?.FindAll().Select(s =>
        {
            var deps = deployments?.Find(d => d.StationId == s.StationId).ToList() ?? new();
            return new DashboardStation
            {
                StationId    = s.StationId,
                StationName  = s.StationName,
                CreatedAt    = s.CreatedAt,
                Online       = s.Online,
                LastOnline   = s.LastOnline,
                Deployments  = deps.Count,
                OnlineDeps   = deps.Count(d => d.Online),
                PlayerCount  = deps.Where(d => d.Online).Sum(d => d.PlayerCount)
            };
        }).ToList() ?? new();

        return Results.Ok(list);
    }

    // DELETE /api/stations/{station_id}?force=true
    //
    // A "fleet" in the dashboard IS a station: v2/fleets on :78 returns fleet_id == StationId, and
    // /api/stations returns the same four rows. StationDb has had DELETE /stations/{id} all along but
    // it is not reachable from the dashboard origin, so the UI had no way to remove a fleet -- which
    // is how duplicate/stale fleets (two "Rigel_1") accumulated with no way to clear them.
    //
    // Guard: a fleet with players on it right now is almost never the one that was meant to be
    // clicked. Refuse unless force=true, and report the count so the UI can ask properly.
    [HttpDelete("/api/stations/{station_id}")]
    public async Task<IHttpActionResult> DeleteStationApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var stations = Program.Database.GetCollection<StationDbObject>(true);
        var station  = stations?.FindOne(s => s.StationId == station_id);
        if (station == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station not found" });

        var deploymentCol = Program.Database.GetCollection<DeploymentDbObject>(true);
        var deps = deploymentCol?.Find(d => d.StationId == station_id).ToList() ?? new();

        bool force = string.Equals(request.GetQueryParameter("force", "") ?? "", "true", StringComparison.OrdinalIgnoreCase);
        int livePlayers = deps.Where(d => d.Online).Sum(d => d.PlayerCount);
        if (livePlayers > 0 && !force)
            return Results.Ok(new DashboardActionResult
            {
                Success = false,
                Error   = $"fleet has {livePlayers} player(s) online — confirm again to delete anyway"
            });

        stations!.Delete(station.Id);

        // Cascade. The EOS sessions matter as much as the rows: leaving them behind keeps a deleted
        // fleet advertised in the station browser with a game server that no longer has a station.
        foreach (var dep in deps)
        {
            EosGatewayServer.RemoveSessionsByDeployment(dep.DeploymentId);
            deploymentCol!.Delete(dep.Id);
        }

        var roleCol = Program.Database.GetCollection<RoleResponse>(true);
        foreach (var role in roleCol?.Find(r => r.StationId == station_id).ToList() ?? new())
            roleCol!.Delete(role.Id);

        var eventCol = Program.Database.GetCollection<StationEventDbObject>(true);
        foreach (var ev in eventCol?.Find(e => e.StationId == station_id).ToList() ?? new())
            eventCol!.Delete(ev.Id);

        return Results.Ok(new DashboardActionResult { Success = true, Id = station_id });
    }

    [HttpGet("/api/deployments")]
    public async Task<IHttpActionResult> GetDeployments(IHttpRequest request, IHttpResponse response)
    {
        var deployments = Program.Database.GetCollection<DeploymentDbObject>(true);
        var list = deployments?.FindAll()
            .OrderByDescending(d => d.LastEvent ?? DateTime.MinValue)
            .Take(100)
            .Select(d => new DashboardDeployment
            {
                DeploymentId   = d.DeploymentId,
                StationId      = d.StationId,
                DeploymentName = d.DeploymentName,
                IpAddress      = d.IpAddress,
                Region         = d.Region,
                Version        = d.Version,
                Online         = d.Online,
                PlayerCount    = d.PlayerCount,
                LastEvent      = d.LastEvent,
                CreatedAt      = d.CreatedAt
            })
            .ToList() ?? new();

        return Results.Ok(list);
    }

    [HttpGet("/api/events")]
    public async Task<IHttpActionResult> GetEvents(IHttpRequest request, IHttpResponse response)
    {
        var events = Program.Database.GetCollection<ServerEventDbObject>(true);
        var list = events?.FindAll()
            .OrderByDescending(e => e.Timestamp)
            .Take(100)
            .Select(e => new DashboardEvent
            {
                EventId      = e.Id.ToString(),
                DeploymentId = e.DeploymentId,
                EventName    = e.EventType,
                CreatedAt    = e.Timestamp
            })
            .ToList() ?? new();

        return Results.Ok(list);
    }

    // ── Station / Deployment / Event creation ────────────────────────────────

    [HttpPost("/api/stations")]
    public async Task<IHttpActionResult> CreateStationApi(IHttpRequest request, IHttpResponse response)
    {
        DashboardCreateStation? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardCreateStation>(request.Body); }
        catch { body = null; }

        if (body == null || string.IsNullOrWhiteSpace(body.StationName))
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station_name required" });

        var stationId = Guid.NewGuid().ToString("N");
        Program.Database.GetCollection<StationDbObject>(true)?.Insert(new StationDbObject
        {
            StationId   = stationId,
            StationName = body.StationName,
            CreatedAt   = DateTime.UtcNow,
            Online      = false,
            Config      = new Dictionary<string, string>()
        });
        return Results.Ok(new DashboardActionResult { Success = true, Id = stationId });
    }

    [HttpPost("/api/stations/{station_id}/deployments")]
    public async Task<IHttpActionResult> CreateDeploymentApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        DashboardCreateDeployment? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardCreateDeployment>(request.Body); }
        catch { body = null; }

        var stationCol = Program.Database.GetCollection<StationDbObject>(true);
        if (stationCol?.FindOne(s => s.StationId == station_id) == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station not found" });

        var deploymentId = Guid.NewGuid().ToString("N");
        var apiKey       = GenerateDashboardApiKey(deploymentId);

        Program.Database.GetCollection<DeploymentDbObject>(true)?.Insert(new DeploymentDbObject
        {
            DeploymentId   = deploymentId,
            StationId      = station_id,
            DeploymentName = body?.DeploymentName,
            IpAddress      = body?.IpAddress,
            Region         = body?.Region,
            Version        = body?.Version,
            CreatedAt      = DateTime.UtcNow,
            Online         = false,
            PlayerCount    = 0,
            Config         = new Dictionary<string, string>(),
            ApiKeys        = new List<string> { apiKey }
        });

        return Results.Ok(new DashboardLaunchResult { Success = true, Id = deploymentId, ApiKey = apiKey });
    }

    // Spin-up: the replacement for the manual "type in ip/port" deployment create. Instead of a human
    // supplying an address, we ask HalcyonSocket to have an allocator agent LAUNCH a game server on a
    // box; that server then SELF-REGISTERS via /register_server and appears in this station's
    // deployment list like any other. Body: { box?, map?, region?, args? }.
    [HttpPost("/api/stations/{station_id}/spinup")]
    public async Task<IHttpActionResult> SpinUpDeployment(IHttpRequest request, IHttpResponse response, string station_id)
    {
        SpinUpBody? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<SpinUpBody>(request.Body); }
        catch { body = null; }

        var stationCol = Program.Database.GetCollection<StationDbObject>(true);
        if (stationCol?.FindOne(s => s.StationId == station_id) == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station not found" });

        if (HalcyonSocketServer.Instance == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "HalcyonSocket not running" });

        var result = HalcyonSocketServer.Instance.RequestSpinUp(body?.box, body?.map, body?.region, body?.args, body?.name);
        return Results.Ok(result);   // { success, request_id, agent, error }
    }

    // Connected allocator agents (game boxes) + their free capacity — for the dashboard's spin-up
    // box picker. Proxied to the dashboard as /api/agents.
    [HttpGet("/api/agents")]
    public async Task<IHttpActionResult> GetAgents(IHttpRequest request, IHttpResponse response)
    {
        var agents = HalcyonSocketServer.Instance?.GetAgents() ?? Array.Empty<AgentInfo>();
        return Results.Ok(new { agents });
    }

    [HttpPost("/api/stations/{station_id}/events")]
    public async Task<IHttpActionResult> CreateEventApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        DashboardCreateEvent? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardCreateEvent>(request.Body); }
        catch { body = null; }

        if (body == null || string.IsNullOrWhiteSpace(body.Title))
            return Results.Ok(new DashboardActionResult { Success = false, Error = "title required" });

        var eventId = Guid.NewGuid().ToString("N");
        Program.Database.GetCollection<StationEventDbObject>(true)?.Insert(new StationEventDbObject
        {
            EventId      = eventId,
            StationId    = station_id,
            DeploymentId = string.IsNullOrWhiteSpace(body.DeploymentId) ? null : body.DeploymentId,
            Title        = body.Title,
            Description  = body.Description ?? "",
            StartTime    = body.StartTime == default ? DateTime.UtcNow : body.StartTime,
            Duration     = body.Duration,
            Public       = body.Public,
            SignupsOpen  = body.SignupsOpen,
            Config       = new Dictionary<string, string>()
        });
        return Results.Ok(new DashboardActionResult { Success = true, Id = eventId });
    }

    [HttpGet("/api/stations/{station_id}/events")]
    public async Task<IHttpActionResult> GetStationEventsApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var col    = Program.Database.GetCollection<StationEventDbObject>(true);
        var events = col?.Find(e => e.StationId == station_id)
            .OrderByDescending(e => e.StartTime)
            .Select(e => new DashboardStationEvent
            {
                EventId      = e.EventId,
                StationId    = e.StationId,
                DeploymentId = e.DeploymentId,
                Title        = e.Title,
                Description  = e.Description,
                StartTime    = e.StartTime,
                Duration     = e.Duration,
                Public       = e.Public,
                SignupsOpen  = e.SignupsOpen
            }).ToList() ?? new();
        return Results.Ok(events);
    }

    [HttpDelete("/api/stations/{station_id}/events/{event_id}")]
    public async Task<IHttpActionResult> DeleteEventApi(IHttpRequest request, IHttpResponse response, string station_id, string event_id)
    {
        var col = Program.Database.GetCollection<StationEventDbObject>(true);
        var ev  = col?.FindOne(e => e.EventId == event_id && e.StationId == station_id);
        if (ev == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "event not found" });
        col!.Delete(ev.Id);
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    [HttpGet("/api/stations/{station_id}/deployments")]
    public async Task<IHttpActionResult> GetStationDeploymentsApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var deps = Program.Database.GetCollection<DeploymentDbObject>(true)
            ?.Find(d => d.StationId == station_id)
            .OrderByDescending(d => d.CreatedAt)
            .Select(d => new DashboardDeployment
            {
                DeploymentId   = d.DeploymentId,
                StationId      = d.StationId,
                DeploymentName = d.DeploymentName,
                IpAddress      = d.IpAddress,
                Region         = d.Region,
                Version        = d.Version,
                Online         = d.Online,
                PlayerCount    = d.PlayerCount,
                LastEvent      = d.LastEvent,
                CreatedAt      = d.CreatedAt
            }).ToList() ?? new();
        return Results.Ok(deps);
    }

    // ── Moderation: Bans ─────────────────────────────────────────────────────

    [HttpPost("/api/users/{user_id}/ban")]
    public async Task<IHttpActionResult> BanUser(IHttpRequest request, IHttpResponse response, string user_id)
    {
        DashboardBanRequest? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardBanRequest>(request.Body); }
        catch { body = null; }

        if (body == null || string.IsNullOrWhiteSpace(body.StationId))
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station_id required" });

        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var user  = users?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        user.Bans ??= new List<BanRequest>();
        user.Bans.Add(new BanRequest
        {
            UserId     = user_id,
            StationId  = body.StationId,
            Reason     = body.Reason ?? "",
            Expiration = body.Duration > 0
                ? DateTime.UtcNow.AddSeconds(body.Duration)
                : DateTime.MaxValue,
            Revoked = false
        });
        users!.Update(user);
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    [HttpPatch("/api/users/{user_id}/unban")]
    public async Task<IHttpActionResult> UnbanUser(IHttpRequest request, IHttpResponse response, string user_id)
    {
        DashboardBanRequest? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardBanRequest>(request.Body); }
        catch { body = null; }

        if (body == null || string.IsNullOrWhiteSpace(body.StationId))
            return Results.Ok(new DashboardActionResult { Success = false, Error = "station_id required" });

        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var user  = users?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        bool changed = false;
        foreach (var ban in user.Bans ?? new List<BanRequest>())
        {
            if (ban.StationId == body.StationId && !ban.Revoked)
            {
                ban.Revoked = true;
                changed = true;
            }
        }
        if (changed) users!.Update(user);
        return Results.Ok(new DashboardActionResult { Success = changed });
    }

    [HttpGet("/api/stations/{station_id}/bans")]
    public async Task<IHttpActionResult> GetStationBans(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var now   = DateTime.UtcNow;
        var bans  = users?.FindAll()
            .SelectMany(u => (u.Bans ?? new List<BanRequest>())
                .Where(b => b.StationId == station_id && !b.Revoked && b.Expiration > now)
                .Select(b => new DashboardBan
                {
                    UserId     = u.UserId,
                    Username   = u.Username,
                    StationId  = b.StationId,
                    Reason     = b.Reason,
                    Expiration = b.Expiration == DateTime.MaxValue ? null : b.Expiration,
                    Revoked    = b.Revoked
                }))
            .ToList() ?? new();

        return Results.Ok(bans);
    }

    // ── Moderation: Roles ────────────────────────────────────────────────────

    [HttpGet("/api/stations/{station_id}/roles")]
    public async Task<IHttpActionResult> GetStationRolesApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var roles = Program.Database.GetCollection<RoleResponse>(true);
        var list  = roles?.Find(r => r.StationId == station_id)
            .Select(r => new DashboardRole
            {
                RoleId          = r.RoleId,
                StationId       = r.StationId,
                RoleName        = r.RoleName,
                RoleDescription = r.RoleDescription,
                Permissions     = r.Permissions ?? new List<string>()
            })
            .ToList() ?? new();

        return Results.Ok(list);
    }

    [HttpPost("/api/stations/{station_id}/roles")]
    public async Task<IHttpActionResult> CreateStationRoleApi(IHttpRequest request, IHttpResponse response, string station_id)
    {
        DashboardCreateRole? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardCreateRole>(request.Body); }
        catch { body = null; }

        if (body == null || string.IsNullOrWhiteSpace(body.RoleName))
            return Results.Ok(new DashboardActionResult { Success = false, Error = "role_name required" });

        var roleId = Guid.NewGuid().ToString("N");
        var role   = new RoleResponse
        {
            RoleId          = roleId,
            StationId       = station_id,
            RoleName        = body.RoleName,
            RoleDescription = body.RoleDescription ?? "",
            Permissions     = new List<string>()
        };
        Program.Database.GetCollection<RoleResponse>(true)?.Insert(role);
        return Results.Ok(new DashboardActionResult { Success = true, Id = roleId });
    }

    [HttpDelete("/api/stations/{station_id}/roles/{role_id}")]
    public async Task<IHttpActionResult> DeleteStationRoleApi(IHttpRequest request, IHttpResponse response, string station_id, string role_id)
    {
        var roles = Program.Database.GetCollection<RoleResponse>(true);
        var role  = roles?.FindOne(r => r.RoleId == role_id && r.StationId == station_id);
        if (role == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "role not found" });

        roles!.Delete(role.Id);

        var users = Program.Database.GetCollection<UserDataResponse>(true);
        foreach (var u in users?.FindAll().Where(u => u.Roles != null && u.Roles.Contains(role_id)).ToList() ?? new())
        {
            u.Roles!.Remove(role_id);
            users!.Update(u);
        }
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    [HttpPatch("/api/stations/{station_id}/roles/{role_id}/permissions")]
    public async Task<IHttpActionResult> UpdateRolePermissionsApi(IHttpRequest request, IHttpResponse response, string station_id, string role_id)
    {
        DashboardSetPermissions? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<DashboardSetPermissions>(request.Body); }
        catch { body = null; }

        var roles = Program.Database.GetCollection<RoleResponse>(true);
        var role  = roles?.FindOne(r => r.RoleId == role_id && r.StationId == station_id);
        if (role == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "role not found" });

        role.Permissions = body?.Permissions ?? role.Permissions;
        roles!.Update(role);
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    // ── Moderation: Role assignment ──────────────────────────────────────────

    [HttpPost("/api/stations/{station_id}/users/{user_id}/roles/{role_id}")]
    public async Task<IHttpActionResult> AssignRoleApi(IHttpRequest request, IHttpResponse response, string station_id, string user_id, string role_id)
    {
        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var user  = users?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        user.Roles ??= new List<string>();
        if (!user.Roles.Contains(role_id))
        {
            user.Roles.Add(role_id);
            users!.Update(user);
        }
        return Results.Ok(new DashboardActionResult { Success = true });
    }

    [HttpDelete("/api/stations/{station_id}/users/{user_id}/roles/{role_id}")]
    public async Task<IHttpActionResult> RemoveRoleApi(IHttpRequest request, IHttpResponse response, string station_id, string user_id, string role_id)
    {
        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var user  = users?.FindOne(u => u.UserId == user_id);
        if (user == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "user not found" });

        bool removed = user.Roles?.Remove(role_id) ?? false;
        if (removed) users!.Update(user);
        return Results.Ok(new DashboardActionResult { Success = removed });
    }

    // ── EOS Sessions (in-memory, managed here, consumed by EosGatewayServer) ──

    [HttpGet("/api/sessions")]
    public async Task<IHttpActionResult> GetSessionsApi(IHttpRequest request, IHttpResponse response)
    {
        List<EosSessionInfo> snapshot;
        lock (EosGatewayServer.Sessions) { snapshot = EosGatewayServer.Sessions.ToList(); }
        return Results.Ok(snapshot);
    }

    [HttpPost("/api/sessions")]
    public async Task<IHttpActionResult> AddSessionApi(IHttpRequest request, IHttpResponse response)
    {
        EosSessionInfo? body;
        try { body = System.Text.Json.JsonSerializer.Deserialize<EosSessionInfo>(request.Body); }
        catch { body = null; }

        if (body == null)
            return Results.Ok(new DashboardActionResult { Success = false, Error = "invalid body" });

        if (string.IsNullOrWhiteSpace(body.Id))
            body.Id = Guid.NewGuid().ToString("N")[..16];

        EosGatewayServer.UpsertSession(body);
        return Results.Ok(new DashboardActionResult { Success = true, Id = body.Id });
    }

    [HttpDelete("/api/sessions/{session_id}")]
    public async Task<IHttpActionResult> RemoveSessionApi(IHttpRequest request, IHttpResponse response, string session_id)
    {
        var removed = EosGatewayServer.RemoveSession(session_id);
        return Results.Ok(new DashboardActionResult { Success = removed });
    }

    // ── Telemetry ─────────────────────────────────────────────────────────────

    [HttpGet("/api/telemetry")]
    public async Task<IHttpActionResult> GetTelemetry(IHttpRequest request, IHttpResponse response)
    {
        request.Queries.TryGetValue("limit", out var limitStr);
        int limit = int.TryParse(limitStr, out var l) ? l : 200;
        var events = AUnrealFeatures.EOSSDK.EosObservabilityStore.GetTelemetry(limit)
            .Select(e => new
            {
                id        = e.Id,
                eventName = e.EventName,
                payload   = e.Payload,
                timestamp = e.Timestamp.ToString("o"),
            });
        return Results.Ok(events);
    }

    // ── WebSocket client monitor ───────────────────────────────────────────────

    [HttpGet("/api/ws-clients")]
    public async Task<IHttpActionResult> GetWsClients(IHttpRequest request, IHttpResponse response)
    {
        var clients = AUnrealFeatures.EOSSDK.EosObservabilityStore.GetClients()
            .Select(c => new
            {
                session_id       = c.SessionId,
                path             = c.Path,
                connected_at     = c.ConnectedAt.ToString("o"),
                disconnected_at  = c.DisconnectedAt?.ToString("o"),
                message_count    = c.MessageCount,
            });
        return Results.Ok(clients);
    }

    [HttpGet("/api/ws-clients/{session_id}/messages")]
    public async Task<IHttpActionResult> GetWsMessages(IHttpRequest request, IHttpResponse response, string session_id)
    {
        var client = AUnrealFeatures.EOSSDK.EosObservabilityStore.GetClient(session_id);
        if (client == null) return Results.NotFound();
        var msgs = client.GetMessages().Select(m => new
        {
            direction = m.Direction,
            command   = m.Command,
            headers   = m.Headers,
            body      = m.Body,
            timestamp = m.Timestamp.ToString("o"),
        });
        return Results.Ok(msgs);
    }

    // ── Station members (users with any role for this station) ───────────────

    [HttpGet("/api/stations/{station_id}/members")]
    public async Task<IHttpActionResult> GetStationMembers(IHttpRequest request, IHttpResponse response, string station_id)
    {
        var roles = Program.Database.GetCollection<RoleResponse>(true);
        var stationRoleIds = roles?.Find(r => r.StationId == station_id)
            .Select(r => r.RoleId).ToHashSet() ?? new HashSet<string>();

        var users = Program.Database.GetCollection<UserDataResponse>(true);
        var members = users?.FindAll()
            .Where(u => u.Roles != null && u.Roles.Any(r => stationRoleIds.Contains(r)))
            .Select(u => new DashboardMember
            {
                UserId   = u.UserId,
                Username = u.Username,
                Platform = u.Platform,
                Roles    = u.Roles!.Where(r => stationRoleIds.Contains(r)).ToList()
            })
            .ToList() ?? new();

        return Results.Ok(members);
    }
}

// ── Response DTOs (dashboard-only, no DB coupling) ───────────────────────────

public sealed class DashboardStats
{
    [JsonPropertyName("total_users")]       public int TotalUsers { get; set; }
    [JsonPropertyName("total_stations")]    public int TotalStations { get; set; }
    [JsonPropertyName("total_deployments")] public int TotalDeployments { get; set; }
    [JsonPropertyName("online_deployments")] public int OnlineDeployments { get; set; }
    [JsonPropertyName("total_players")]     public int TotalPlayers { get; set; }
    [JsonPropertyName("total_events")]      public int TotalEvents { get; set; }
}

public sealed class DashboardUser
{
    [JsonPropertyName("user_id")]    public string UserId { get; set; } = string.Empty;
    [JsonPropertyName("username")]   public string Username { get; set; } = string.Empty;
    [JsonPropertyName("platform")]   public string? Platform { get; set; }
    [JsonPropertyName("last_login")] public DateTime LastLogin { get; set; }
    [JsonPropertyName("created_at")] public DateTime CreatedAt { get; set; }
    [JsonPropertyName("role_count")] public int RoleCount { get; set; }
    [JsonPropertyName("banned")]     public bool Banned { get; set; }
    [JsonPropertyName("is_admin")]   public bool IsAdmin { get; set; }
}

public sealed class DashboardStation
{
    [JsonPropertyName("station_id")]   public string StationId { get; set; } = string.Empty;
    [JsonPropertyName("station_name")] public string StationName { get; set; } = string.Empty;
    [JsonPropertyName("created_at")]   public DateTime CreatedAt { get; set; }
    [JsonPropertyName("online")]       public bool Online { get; set; }
    [JsonPropertyName("last_online")]  public DateTime? LastOnline { get; set; }
    [JsonPropertyName("deployments")]  public int Deployments { get; set; }
    [JsonPropertyName("online_deps")]  public int OnlineDeps { get; set; }
    [JsonPropertyName("player_count")] public int PlayerCount { get; set; }
}

public sealed class DashboardDeployment
{
    [JsonPropertyName("deployment_id")]   public string DeploymentId { get; set; } = string.Empty;
    [JsonPropertyName("station_id")]      public string StationId { get; set; } = string.Empty;
    [JsonPropertyName("deployment_name")] public string? DeploymentName { get; set; }
    [JsonPropertyName("ip_address")]      public string? IpAddress { get; set; }
    [JsonPropertyName("region")]          public string? Region { get; set; }
    [JsonPropertyName("version")]         public string? Version { get; set; }
    [JsonPropertyName("online")]          public bool Online { get; set; }
    [JsonPropertyName("player_count")]    public int PlayerCount { get; set; }
    [JsonPropertyName("last_event")]      public DateTime? LastEvent { get; set; }
    [JsonPropertyName("created_at")]      public DateTime CreatedAt { get; set; }
}

public sealed class DashboardEvent
{
    [JsonPropertyName("event_id")]      public string EventId { get; set; } = string.Empty;
    [JsonPropertyName("deployment_id")] public string DeploymentId { get; set; } = string.Empty;
    [JsonPropertyName("event_name")]    public string EventName { get; set; } = string.Empty;
    [JsonPropertyName("created_at")]    public DateTime CreatedAt { get; set; }
}

public sealed class DashboardActionResult
{
    [JsonPropertyName("success")] public bool Success { get; set; }
    [JsonPropertyName("error")]   public string? Error { get; set; }
    [JsonPropertyName("id")]      public string? Id { get; set; }
}

public sealed class DashboardBanRequest
{
    [JsonPropertyName("station_id")] public string? StationId { get; set; }
    [JsonPropertyName("reason")]     public string? Reason { get; set; }
    [JsonPropertyName("duration")]   public int Duration { get; set; }
}

public sealed class DashboardBan
{
    [JsonPropertyName("user_id")]    public string UserId { get; set; } = string.Empty;
    [JsonPropertyName("username")]   public string Username { get; set; } = string.Empty;
    [JsonPropertyName("station_id")] public string StationId { get; set; } = string.Empty;
    [JsonPropertyName("reason")]     public string Reason { get; set; } = string.Empty;
    [JsonPropertyName("expiration")] public DateTime? Expiration { get; set; }
    [JsonPropertyName("revoked")]    public bool Revoked { get; set; }
}

public sealed class DashboardRole
{
    [JsonPropertyName("role_id")]          public string RoleId { get; set; } = string.Empty;
    [JsonPropertyName("station_id")]       public string StationId { get; set; } = string.Empty;
    [JsonPropertyName("role_name")]        public string RoleName { get; set; } = string.Empty;
    [JsonPropertyName("role_description")] public string RoleDescription { get; set; } = string.Empty;
    [JsonPropertyName("permissions")]      public List<string> Permissions { get; set; } = new();
}

public sealed class DashboardCreateRole
{
    [JsonPropertyName("role_name")]        public string? RoleName { get; set; }
    [JsonPropertyName("role_description")] public string? RoleDescription { get; set; }
}

public sealed class DashboardSetPermissions
{
    [JsonPropertyName("permissions")] public List<string>? Permissions { get; set; }
}

public sealed class DashboardCreateStation
{
    [JsonPropertyName("station_name")] public string? StationName { get; set; }
}

public sealed class DashboardCreateDeployment
{
    [JsonPropertyName("deployment_name")] public string? DeploymentName { get; set; }
    [JsonPropertyName("ip")]              public string? IpAddress { get; set; }
    [JsonPropertyName("region")]          public string? Region { get; set; }
    [JsonPropertyName("version")]         public string? Version { get; set; }
}

public sealed class DashboardLaunchResult
{
    [JsonPropertyName("success")] public bool Success { get; set; }
    [JsonPropertyName("id")]      public string? Id { get; set; }
    [JsonPropertyName("api_key")] public string? ApiKey { get; set; }
}

public sealed class DashboardCreateEvent
{
    [JsonPropertyName("title")]         public string? Title { get; set; }
    [JsonPropertyName("description")]   public string? Description { get; set; }
    [JsonPropertyName("start_time")]    public DateTime StartTime { get; set; }
    [JsonPropertyName("duration")]      public int Duration { get; set; }
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
    [JsonPropertyName("public")]        public bool Public { get; set; }
    [JsonPropertyName("signups_open")]  public bool SignupsOpen { get; set; }
}

public sealed class DashboardStationEvent
{
    [JsonPropertyName("event_id")]      public string EventId { get; set; } = string.Empty;
    [JsonPropertyName("station_id")]    public string StationId { get; set; } = string.Empty;
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
    [JsonPropertyName("title")]         public string Title { get; set; } = string.Empty;
    [JsonPropertyName("description")]   public string Description { get; set; } = string.Empty;
    [JsonPropertyName("start_time")]    public DateTime StartTime { get; set; }
    [JsonPropertyName("duration")]      public int Duration { get; set; }
    [JsonPropertyName("public")]        public bool Public { get; set; }
    [JsonPropertyName("signups_open")]  public bool SignupsOpen { get; set; }
}

public sealed class DashboardMember
{
    [JsonPropertyName("user_id")]  public string UserId { get; set; } = string.Empty;
    [JsonPropertyName("username")] public string Username { get; set; } = string.Empty;
    [JsonPropertyName("platform")] public string? Platform { get; set; }
    [JsonPropertyName("roles")]    public List<string> Roles { get; set; } = new();
}
