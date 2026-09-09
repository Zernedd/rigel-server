using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.AAMothership.Models;
using AUnrealFeatures.Hosting.Database.Interfaces;
using System.Collections.Concurrent;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.AAMothership;

public interface IMothershipServer { }

public sealed class MothershipServer : AstraHttpServer, IMothershipServer
{
    const string HOSTNAME      = "5.175.213.174";
    const ushort PORT          = 9090;
    const string JWT_SECRET    = "your-secret-key-change-this";
    const string ENVIRONMENT_ID = "7f3a99dd-5598-4725-98cf-6538d28feb9f";
    const string TENANT_ID     = "f3e9fb19";

    // Set by AUnrealFeatures.Ares.Program before the server starts
    public static IDatabase Database { get; set; } = null!;

    // In-memory nonce storage for QUEST v2 auth begin/complete flow
    static readonly ConcurrentDictionary<string, string> _pendingQuestAuth = new();

    public MothershipServer() : base(HOSTNAME, PORT) { }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private static string NewId() => Guid.NewGuid().ToString();

    private static long ExpirationSec(int hours = 24)  => DateTimeOffset.UtcNow.AddHours(hours).ToUnixTimeSeconds();
    private static long ExpirationMs(int hours  = 1)   => DateTimeOffset.UtcNow.AddHours(hours).ToUnixTimeMilliseconds();

    private static bool IsAuthorized(IHttpRequest req) =>
        req.GetHeaderValue("x-mothership-token")  != null ||
        req.GetHeaderValue("x-server-api-key")    != null ||
        req.GetHeaderValue("x-automation-key")    != null;

    private static Dictionary<string, JsonElement> ParseBody(IHttpRequest req)
    {
        try { return JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(req.Body.AsSpan()) ?? new(); }
        catch { return new(); }
    }

    private static string  Str(Dictionary<string, JsonElement> b, string key) =>
        b.TryGetValue(key, out var e) && e.ValueKind == JsonValueKind.String ? e.GetString() ?? "" : "";

    private static int     Int(Dictionary<string, JsonElement> b, string key, int def = 0) =>
        b.TryGetValue(key, out var e) && e.ValueKind == JsonValueKind.Number ? e.GetInt32() : def;

    private static bool    Bool(Dictionary<string, JsonElement> b, string key, bool def = false)
    {
        if (!b.TryGetValue(key, out var e)) return def;
        if (e.ValueKind == JsonValueKind.True)  return true;
        if (e.ValueKind == JsonValueKind.False) return false;
        return def;
    }

    private static List<string> StrList(Dictionary<string, JsonElement> b, string key)
    {
        if (!b.TryGetValue(key, out var e) || e.ValueKind != JsonValueKind.Array) return new();
        return e.EnumerateArray()
                .Where(x => x.ValueKind == JsonValueKind.String)
                .Select(x => x.GetString()!)
                .ToList();
    }

    private static string GenerateJwt(object payload)
    {
        var header  = Base64Url(JsonSerializer.SerializeToUtf8Bytes(new { alg = "HS256", typ = "JWT" }));
        var body    = Base64Url(JsonSerializer.SerializeToUtf8Bytes(payload));
        var signing = $"{header}.{body}";
        using var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(JWT_SECRET));
        return $"{signing}.{Base64Url(hmac.ComputeHash(Encoding.UTF8.GetBytes(signing)))}";
    }

    private static string Base64Url(byte[] input) =>
        Convert.ToBase64String(input).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    private static IHttpActionResult Json<T>(T obj, HttpStatusCode code = HttpStatusCode.OK) =>
        Results.Configurable(code, "application/json", JsonSerializer.SerializeToUtf8Bytes<T>(obj));

    private static IHttpActionResult JsonAnon(object obj, HttpStatusCode code = HttpStatusCode.OK) =>
        Results.Configurable(code, "application/json", JsonSerializer.SerializeToUtf8Bytes(obj));

    // Generates a v1-style session JWT (no external service, 24h expiry)
    private static string GenerateSessionJwt(string playerId, string? externalService = null, string? externalServiceId = null)
    {
        var now    = DateTimeOffset.UtcNow;
        var expSec = now.AddHours(24).ToUnixTimeSeconds();
        return GenerateJwt(new
        {
            sub                        = playerId,
            did                        = NewId(),
            env                        = ENVIRONMENT_ID,
            externalService,
            externalServiceId,
            tid                        = TENANT_ID,
            tags                       = (string?)null,
            orgScopedExternalServiceId = externalServiceId,
            nbf                        = now.ToUnixTimeSeconds(),
            exp                        = expSec,
            iat                        = now.ToUnixTimeSeconds()
        });
    }

    // ── CLIENT AUTH V1 ────────────────────────────────────────────────────────

    /// <summary>Creates an anonymous player session using insecure auth method 1.</summary>
    [HttpPost("/v1/client/player/auth/Insecure_1")]
    public async Task<IHttpActionResult> ClientAuthInsecure1(IHttpRequest request, IHttpResponse response)
    {
        var body     = ParseBody(request);
        var playerId = NewId();
        var token    = GenerateSessionJwt(playerId);
        var expSec   = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds();

        var player = new MothershipPlayerDbObject
        {
            MothershipPlayerId      = playerId,
            ExternalAccountNickname = Str(body, "username"),
            ExternalAccountId       = Str(body, "accountId"),
            ExpirationTime          = expSec,
            Tags                    = new(),
            Token                   = token
        };

        Database.GetCollection<MothershipPlayerDbObject>(true)!.Insert(player);
        Database.GetCollection<MothershipSessionDbObject>(true)!
            .Insert(new MothershipSessionDbObject { Token = token, PlayerId = playerId });

        return Json(player);
    }

    /// <summary>Creates an anonymous player session using insecure auth method 2.</summary>
    [HttpPost("/v1/client/player/auth/Insecure_2")]
    public async Task<IHttpActionResult> ClientAuthInsecure2(IHttpRequest request, IHttpResponse response)
    {
        var body     = ParseBody(request);
        var playerId = NewId();
        var token    = GenerateSessionJwt(playerId);
        var expSec   = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds();

        var player = new MothershipPlayerDbObject
        {
            MothershipPlayerId      = playerId,
            ExternalAccountNickname = Str(body, "username"),
            ExternalAccountId       = Str(body, "accountId"),
            ExpirationTime          = expSec,
            Tags                    = new(),
            Token                   = token
        };

        Database.GetCollection<MothershipPlayerDbObject>(true)!.Insert(player);
        Database.GetCollection<MothershipSessionDbObject>(true)!
            .Insert(new MothershipSessionDbObject { Token = token, PlayerId = playerId });

        return Json(player);
    }

    /// <summary>Authenticates a player via Google external ID.</summary>
    [HttpPost("/v1/client/player/auth/GOOGLE")]
    public async Task<IHttpActionResult> ClientAuthGoogle(IHttpRequest request, IHttpResponse response)
    {
        var body       = ParseBody(request);
        var playerId   = NewId();
        var externalId = Str(body, "UserId");
        var token      = GenerateSessionJwt(playerId, "GOOGLE", externalId);

        var player = new MothershipPlayerDbObject
        {
            MothershipPlayerId      = playerId,
            ExternalAccountNickname = "",
            ExternalAccountId       = externalId,
            ExpirationTime          = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds(),
            Tags                    = new(),
            Token                   = token
        };
        Database.GetCollection<MothershipPlayerDbObject>(true)!.Insert(player);
        return Json(player);
    }

    /// <summary>Authenticates a player via Apple Game Center ID.</summary>
    [HttpPost("/v1/client/player/auth/APPLE")]
    public async Task<IHttpActionResult> ClientAuthApple(IHttpRequest request, IHttpResponse response)
    {
        var body       = ParseBody(request);
        var playerId   = NewId();
        var externalId = Str(body, "GamePlayerId");
        var token      = GenerateSessionJwt(playerId, "APPLE", externalId);

        var player = new MothershipPlayerDbObject
        {
            MothershipPlayerId      = playerId,
            ExternalAccountNickname = "",
            ExternalAccountId       = externalId,
            ExpirationTime          = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds(),
            Tags                    = new(),
            Token                   = token
        };
        Database.GetCollection<MothershipPlayerDbObject>(true)!.Insert(player);
        return Json(player);
    }

    /// <summary>Authenticates a player via Meta Quest.</summary>
    [HttpPost("/v1/client/player/auth/QUEST")]
    public async Task<IHttpActionResult> ClientAuthQuest(IHttpRequest request, IHttpResponse response)
    {
        var playerId = NewId();
        var token    = GenerateSessionJwt(playerId, "QUEST");

        var player = new MothershipPlayerDbObject
        {
            MothershipPlayerId      = playerId,
            ExternalAccountNickname = "",
            ExternalAccountId       = "",
            ExpirationTime          = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds(),
            Tags                    = new(),
            Token                   = token
        };
        Database.GetCollection<MothershipPlayerDbObject>(true)!.Insert(player);
        return Json(player);
    }

    /// <summary>Authenticates a player via Oculus Rift with a fixed external ID.</summary>
    [HttpPost("/v1/client/player/auth/RIFT")]
    public async Task<IHttpActionResult> ClientAuthRift(IHttpRequest request, IHttpResponse response)
    {
        const string RIFT_EXTERNAL_ID      = "4649330831842445";
        const string RIFT_ORG_SCOPED_ID    = "4322323827863641";

        var playerId = NewId();
        var now      = DateTimeOffset.UtcNow;
        var expSec   = now.AddHours(1).ToUnixTimeSeconds();
        var expMs    = now.AddHours(1).ToUnixTimeMilliseconds();

        var token = GenerateJwt(new
        {
            sub                        = playerId,
            did                        = NewId(),
            env                        = ENVIRONMENT_ID,
            externalService            = "RIFT",
            externalServiceId          = RIFT_EXTERNAL_ID,
            tid                        = TENANT_ID,
            tags                       = (string?)null,
            orgScopedExternalServiceId = RIFT_ORG_SCOPED_ID,
            nbf                        = now.ToUnixTimeSeconds(),
            exp                        = expSec,
            iat                        = now.ToUnixTimeSeconds()
        });

        var player = new MothershipV2PlayerDbObject
        {
            ExternalProviderId      = RIFT_EXTERNAL_ID,
            ExternalProviderUsername = "",
            IsPrimaryId             = true,
            PlayerId                = playerId,
            Tags                    = null,
            Token                   = token,
            ExpirationTime          = expMs,
            ExternalService         = "RIFT"
        };
        Database.GetCollection<MothershipV2PlayerDbObject>(true)!.Insert(player);

        return Json(player, HttpStatusCode.Created);
    }

    // ── ANALYTICS ─────────────────────────────────────────────────────────────

    /// <summary>Accepts a client analytics event batch (no-op, returns empty result).</summary>
    [HttpPost("/v1/client/analytics/event/batch")]
    public async Task<IHttpActionResult> AnalyticsBatch(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new { event_id = "not found", results = Array.Empty<object>() });
    }

    // ── AUTH V2 — QUEST ───────────────────────────────────────────────────────

    /// <summary>Begins the Quest v2 auth flow, returns an attestation nonce.</summary>
    [HttpPost("/v2/player/client/auth/begin/QUEST")]
    public async Task<IHttpActionResult> BeginQuestAuth(IHttpRequest request, IHttpResponse response)
    {
        var body   = ParseBody(request);
        var userId = Str(body, "UserId");
        if (string.IsNullOrEmpty(userId))
            return JsonAnon(new { error = "UserId not found" }, HttpStatusCode.NotFound);

        var nonce = Base64Url(RandomNumberGenerator.GetBytes(40));
        _pendingQuestAuth[userId] = nonce;

        return JsonAnon(new { AttestationNonce = nonce });
    }

    /// <summary>Completes the Quest v2 auth flow and issues a JWT.</summary>
    [HttpPost("/v2/player/client/auth/complete/QUEST")]
    public async Task<IHttpActionResult> CompleteQuestAuth(IHttpRequest request, IHttpResponse response)
    {
        var body             = ParseBody(request);
        var userId           = Str(body, "UserId");
        var attestationToken = Str(body, "AttestationToken");
        var metaNonce        = Str(body, "MetaNonce");

        if (string.IsNullOrEmpty(userId))           return JsonAnon(new { error = "UserId not found" },           HttpStatusCode.NotFound);
        if (string.IsNullOrEmpty(attestationToken)) return JsonAnon(new { error = "AttestationToken not found" }, HttpStatusCode.NotFound);
        if (string.IsNullOrEmpty(metaNonce))        return JsonAnon(new { error = "MetaNonce not found" },        HttpStatusCode.NotFound);
        if (!_pendingQuestAuth.ContainsKey(userId)) return JsonAnon(new { error = "No pending authentication" },  HttpStatusCode.NotFound);

        var col      = Database.GetCollection<MothershipV2PlayerDbObject>(true)!;
        var existing = col.FindOne(p => p.ExternalProviderId == userId && p.ExternalService == "QUEST");
        var playerId = existing?.PlayerId ?? NewId();

        var now    = DateTimeOffset.UtcNow;
        var expSec = now.AddHours(1).ToUnixTimeSeconds();
        var expMs  = now.AddHours(1).ToUnixTimeMilliseconds();

        var jwtPayload = new
        {
            sub                         = playerId,
            did                         = NewId(),
            env                         = ENVIRONMENT_ID,
            externalService             = "QUEST",
            externalServiceId           = userId,
            tid                         = TENANT_ID,
            tags                        = (string?)null,
            orgScopedExternalServiceId  = userId,
            nbf                         = now.ToUnixTimeSeconds(),
            exp                         = expSec,
            iat                         = now.ToUnixTimeSeconds()
        };

        var player = existing ?? new MothershipV2PlayerDbObject();
        player.ExternalProviderId      = userId;
        player.ExternalProviderUsername = $"Player_{userId[..Math.Min(8, userId.Length)]}";
        player.IsPrimaryId             = true;
        player.PlayerId                = playerId;
        player.Tags                    = null;
        player.Token                   = GenerateJwt(jwtPayload);
        player.ExpirationTime          = expMs;
        player.ExternalService         = "QUEST";

        if (existing == null) col.Insert(player);
        else                  col.Update(player);

        _pendingQuestAuth.TryRemove(userId, out _);
        return Json(player, HttpStatusCode.Created);
    }

    // ── AUTH V2 — STEAM ───────────────────────────────────────────────────────

    /// <summary>Begins the Steam v2 auth flow, returns a nonce.</summary>
    [HttpGet("/v2/player/client/auth/begin/STEAM")]
    public async Task<IHttpActionResult> BeginSteamAuth(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new { Nonce = Base64Url(RandomNumberGenerator.GetBytes(16)) });
    }

    /// <summary>Completes the Steam v2 auth flow and issues a JWT.</summary>
    [HttpPost("/v2/player/client/auth/complete/STEAM")]
    public async Task<IHttpActionResult> CompleteSteamAuth(IHttpRequest request, IHttpResponse response)
    {
        var body        = ParseBody(request);
        var nonce       = Str(body, "Nonce");
        var steamTicket = Str(body, "SteamTicket");

        if (string.IsNullOrEmpty(nonce))       return JsonAnon(new { error = "Nonce not found" },       HttpStatusCode.NotFound);
        if (string.IsNullOrEmpty(steamTicket)) return JsonAnon(new { error = "SteamTicket not found" }, HttpStatusCode.NotFound);

        var steamId    = "76561199232028535"; // validate with Steam API in production
        var playerId   = NewId();
        var deployId   = request.GetHeaderValue("x-mothership-deployment-id") ?? NewId();
        var envId      = request.GetHeaderValue("x-mothership-env-id")        ?? ENVIRONMENT_ID;
        var titleId    = request.GetHeaderValue("x-mothership-title-id")      ?? TENANT_ID;

        var now    = DateTimeOffset.UtcNow;
        var expSec = now.AddHours(1).ToUnixTimeSeconds();
        var expMs  = now.AddHours(1).ToUnixTimeMilliseconds();

        var jwtPayload = new
        {
            sub                        = playerId,
            did                        = deployId,
            env                        = envId,
            externalService            = "STEAM",
            externalServiceId          = steamId,
            tid                        = titleId,
            tags                       = (string?)null,
            orgScopedExternalServiceId = steamId,
            nbf                        = now.ToUnixTimeSeconds(),
            exp                        = expSec,
            iat                        = now.ToUnixTimeSeconds()
        };

        var player = new MothershipV2PlayerDbObject
        {
            ExternalProviderId      = steamId,
            ExternalProviderUsername = "drycheetah84",
            IsPrimaryId             = true,
            PlayerId                = playerId,
            Tags                    = null,
            Token                   = GenerateJwt(jwtPayload),
            ExpirationTime          = expMs,
            ExternalService         = "STEAM"
        };

        Database.GetCollection<MothershipV2PlayerDbObject>(true)!.Insert(player);
        return Json(player, HttpStatusCode.Created);
    }

    // ── SERVER AUTH ───────────────────────────────────────────────────────────

    /// <summary>Verifies a player session token and returns the associated player.</summary>
    [HttpPost("/v1/server/player/auth/verify_token")]
    public async Task<IHttpActionResult> VerifyToken(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var token   = Str(body, "token");
        var session = Database.GetCollection<MothershipSessionDbObject>(true)!.FindOne(s => s.Token == token);
        if (session == null) return JsonAnon(new { valid = false }, HttpStatusCode.Unauthorized);

        var player = Database.GetCollection<MothershipPlayerDbObject>(true)!.FindOne(p => p.MothershipPlayerId == session.PlayerId);
        if (player == null) return JsonAnon(new { valid = false }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { valid = true, player });
    }

    /// <summary>Creates a platform association for a player.</summary>
    [HttpPost("/v1/server/player/auth/association")]
    public async Task<IHttpActionResult> CreateAssociation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body  = ParseBody(request);
        var assoc = new MothershipAssociationDbObject
        {
            AssociationId = NewId(),
            PlayerId      = Str(body, "player_id"),
            Platform      = Str(body, "platform"),
            CreatedAt     = DateTime.UtcNow
        };
        Database.GetCollection<MothershipAssociationDbObject>(true)!.Insert(assoc);
        return Json(assoc);
    }

    /// <summary>Returns all associations for a given player ID.</summary>
    [HttpGet("/v1/server/player/auth/associations/{player_id}")]
    public async Task<IHttpActionResult> GetAssociations(IHttpRequest request, IHttpResponse response, string player_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipAssociationDbObject>(true)!
            .Find(a => a.PlayerId == player_id).ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes all associations for a given player ID.</summary>
    [HttpDelete("/v1/server/player/auth/associations/{player_id}")]
    public async Task<IHttpActionResult> DeleteAssociations(IHttpRequest request, IHttpResponse response, string player_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        Database.GetCollection<MothershipAssociationDbObject>(true)!.DeleteMany(a => a.PlayerId == player_id);
        return JsonAnon(new { status = "deleted" });
    }

    // ── AUTOMATION AUTH ───────────────────────────────────────────────────────

    /// <summary>Lists all players (automation).</summary>
    [HttpGet("/v1/automation/player/auth/players")]
    public async Task<IHttpActionResult> AutomationGetPlayers(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipPlayerDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Validates a username and returns a placeholder user (automation).</summary>
    [HttpPost("/v1/automation/player/auth/players")]
    public async Task<IHttpActionResult> AutomationValidatePlayer(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body     = ParseBody(request);
        var username = Str(body, "username");
        return JsonAnon(new { username, user_id = NewId(), recent_flagged_usernames = Array.Empty<string>() });
    }

    /// <summary>Updates tags on a player record (automation).</summary>
    [HttpPost("/v1/automation/player/auth/players/tags")]
    public async Task<IHttpActionResult> AutomationUpdatePlayerTags(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body     = ParseBody(request);
        var playerId = Str(body, "player_id");
        var tags     = StrList(body, "tags");
        var col      = Database.GetCollection<MothershipPlayerDbObject>(true)!;
        var player   = col.FindOne(p => p.MothershipPlayerId == playerId);
        if (player == null) return JsonAnon(new { error = "Player not found" }, HttpStatusCode.NotFound);

        player.Tags = tags;
        col.Update(player);
        return JsonAnon(new { playersToTags = new Dictionary<string, List<string>> { [playerId] = tags } });
    }

    /// <summary>Creates an account link between players (automation).</summary>
    [HttpPost("/v1/automation/player/auth/links")]
    public async Task<IHttpActionResult> AutomationCreateLink(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var link = new MothershipLinkDbObject
        {
            LinkId       = NewId(),
            Platform     = "",
            TargetPlayer = Str(body, "TargetPlayer"),
            OtherToken   = Str(body, "OtherToken"),
            IsPrimary    = false,
            CreatedAt    = DateTime.UtcNow
        };
        Database.GetCollection<MothershipLinkDbObject>(true)!.Insert(link);
        return Json(link);
    }

    /// <summary>Marks an account link as primary (automation).</summary>
    [HttpPost("/v1/automation/player/auth/links/set_primary")]
    public async Task<IHttpActionResult> AutomationSetPrimaryLink(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var linkId = Str(body, "link_id");
        var col    = Database.GetCollection<MothershipLinkDbObject>(true)!;
        var link   = col.FindOne(l => l.LinkId == linkId);
        if (link == null) return JsonAnon(new { error = "Link not found" }, HttpStatusCode.NotFound);

        link.IsPrimary = true;
        col.Update(link);
        return Json(link);
    }

    /// <summary>Returns account links for a player on a given platform (automation).</summary>
    [HttpGet("/v1/automation/player/auth/links/{platform}")]
    public async Task<IHttpActionResult> AutomationGetLinks(IHttpRequest request, IHttpResponse response, string platform)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var playerId = request.GetQueryParameter("player_id") ?? "";
        var links    = Database.GetCollection<MothershipLinkDbObject>(true)!
            .Find(l => l.TargetPlayer == playerId).ToList();
        return JsonAnon(new { Identities = links });
    }

    /// <summary>Deletes an account link by ID (automation).</summary>
    [HttpDelete("/v1/automation/player/auth/links/{platform}/{link_id}")]
    public async Task<IHttpActionResult> AutomationDeleteLink(IHttpRequest request, IHttpResponse response, string platform, string link_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var col  = Database.GetCollection<MothershipLinkDbObject>(true)!;
        var link = col.FindOne(l => l.LinkId == link_id);
        if (link == null) return JsonAnon(new { error = "Link not found" }, HttpStatusCode.NotFound);

        col.Delete(link.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Returns all associations for a Mothership player ID (automation).</summary>
    [HttpGet("/v1/automation/player/auth/player/all_associations")]
    public async Task<IHttpActionResult> AutomationAllAssociations(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var mothershipId = request.GetQueryParameter("mothershipId") ?? "";
        var results      = Database.GetCollection<MothershipAssociationDbObject>(true)!
            .Find(a => a.PlayerId == mothershipId).ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a specific association by ID (automation).</summary>
    [HttpDelete("/v1/automation/player/auth/associations/{platform}/{assoc_id}")]
    public async Task<IHttpActionResult> AutomationDeleteAssociation(IHttpRequest request, IHttpResponse response, string platform, string assoc_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var col   = Database.GetCollection<MothershipAssociationDbObject>(true)!;
        var assoc = col.FindOne(a => a.AssociationId == assoc_id);
        if (assoc == null) return JsonAnon(new { error = "Association not found" }, HttpStatusCode.NotFound);

        col.Delete(assoc.Id);
        return JsonAnon(new { status = "deleted" });
    }

    // ── EXPLICIT LINK ─────────────────────────────────────────────────────────

    /// <summary>Explicitly links two accounts on a given platform.</summary>
    [HttpPost("/v1/{platform}/player/auth/explicit_link")]
    public async Task<IHttpActionResult> ExplicitLink(IHttpRequest request, IHttpResponse response, string platform)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var link = new MothershipLinkDbObject
        {
            LinkId       = NewId(),
            Platform     = platform,
            TargetPlayer = Str(body, "TargetPlayer"),
            OtherToken   = Str(body, "OtherToken"),
            IsPrimary    = false,
            CreatedAt    = DateTime.UtcNow
        };
        Database.GetCollection<MothershipLinkDbObject>(true)!.Insert(link);
        return JsonAnon(new { link_id = link.LinkId, status = "linked" });
    }

    // ── MODERATION ────────────────────────────────────────────────────────────

    /// <summary>Submits a moderation report from a client.</summary>
    [HttpPost("/v1/moderation/client/report")]
    public async Task<IHttpActionResult> ClientCreateReport(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var report = new MothershipReportDbObject
        {
            ReportId       = NewId(),
            ReportedUserId = Str(body, "reported_user_id"),
            ReporterUserId = "",
            Category       = Int(body, "category"),
            Reason         = "",
            Platform       = Str(body, "platform"),
            ModdedClient   = Bool(body, "modded_client"),
            Metadata       = Str(body, "metadata"),
            CreatedAt      = DateTime.UtcNow
        };
        Database.GetCollection<MothershipReportDbObject>(true)!.Insert(report);
        return Json(report);
    }

    /// <summary>Submits a moderation report from a server.</summary>
    [HttpPost("/v1/moderation/server/report")]
    public async Task<IHttpActionResult> ServerCreateReport(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var report = new MothershipReportDbObject
        {
            ReportId       = NewId(),
            ReportedUserId = Str(body, "reported_user_id"),
            ReporterUserId = Str(body, "reporter_user_id"),
            Category       = Int(body, "category"),
            Reason         = Str(body, "reason"),
            Platform       = "",
            ModdedClient   = false,
            Metadata       = Str(body, "metadata"),
            CreatedAt      = DateTime.UtcNow
        };
        Database.GetCollection<MothershipReportDbObject>(true)!.Insert(report);
        return Json(report);
    }

    /// <summary>Bans a player for a specified duration.</summary>
    [HttpPost("/v1/moderation/server/ban")]
    public async Task<IHttpActionResult> ServerCreateBan(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var minutes = Int(body, "duration_minutes");
        var ban     = new MothershipBanDbObject
        {
            BanId           = NewId(),
            TitleId         = Str(body, "title_id"),
            EnvId           = Str(body, "env_id"),
            PlayerId        = Str(body, "player_id"),
            Category        = Int(body, "category"),
            Reason          = Str(body, "reason"),
            DurationMinutes = minutes,
            OrgWide         = Bool(body, "org_wide"),
            Metadata        = Str(body, "metadata"),
            CreatedAt       = DateTime.UtcNow,
            ExpiresAt       = DateTime.UtcNow.AddMinutes(minutes)
        };
        Database.GetCollection<MothershipBanDbObject>(true)!.Insert(ban);
        return JsonAnon(new { Results = new[] { ban } });
    }

    /// <summary>Bans multiple players in a single request.</summary>
    [HttpPost("/v1/moderation/server/bans/bulk")]
    public async Task<IHttpActionResult> ServerBulkBans(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var bans = new List<MothershipBanDbObject>();
        var col  = Database.GetCollection<MothershipBanDbObject>(true)!;

        if (body.TryGetValue("players", out var playersEl) && playersEl.ValueKind == JsonValueKind.Array)
        {
            foreach (var p in playersEl.EnumerateArray())
            {
                var minutes = p.TryGetProperty("duration_minutes", out var dm) ? dm.GetInt32() : 0;
                var ban     = new MothershipBanDbObject
                {
                    BanId           = NewId(),
                    PlayerId        = p.TryGetProperty("player_id", out var pid) ? pid.GetString() ?? "" : "",
                    Category        = p.TryGetProperty("category",  out var cat) ? cat.GetInt32() : 0,
                    Reason          = p.TryGetProperty("reason",    out var r)   ? r.GetString()  ?? "" : "",
                    DurationMinutes = minutes,
                    CreatedAt       = DateTime.UtcNow,
                    ExpiresAt       = DateTime.UtcNow.AddMinutes(minutes)
                };
                col.Insert(ban);
                bans.Add(ban);
            }
        }
        return JsonAnon(new { Results = bans });
    }

    /// <summary>Lists bans and/or reports, optionally filtered by IDs.</summary>
    [HttpGet("/v1/moderation")]
    public async Task<IHttpActionResult> ListModeration(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var banId    = request.GetQueryParameter("ban_id");
        var playerId = request.GetQueryParameter("player_id");
        var reportId = request.GetQueryParameter("report_id");

        if (banId != null)
        {
            var ban = Database.GetCollection<MothershipBanDbObject>(true)!.FindOne(b => b.BanId == banId);
            if (ban != null) return Json(ban);
        }
        else if (reportId != null)
        {
            var report = Database.GetCollection<MothershipReportDbObject>(true)!.FindOne(r => r.ReportId == reportId);
            if (report != null) return JsonAnon(new { Report = report });
        }
        else if (playerId != null)
        {
            var bans = Database.GetCollection<MothershipBanDbObject>(true)!.Find(b => b.PlayerId == playerId).ToList();
            return JsonAnon(new { Results = bans });
        }
        else
        {
            var allBans    = Database.GetCollection<MothershipBanDbObject>(true)!.FindAll().ToList();
            var allReports = Database.GetCollection<MothershipReportDbObject>(true)!.FindAll().ToList();
            return JsonAnon(new { bans = allBans, reports = allReports });
        }

        return Results.NotFound();
    }

    // Moderation automation routes — declared before the generic {platform}/{group_id} routes.
    // The generic handlers also delegate here if platform=="moderation" && group_id=="automation".

    /// <summary>Returns all bans, mutes and reports (automation).</summary>
    [HttpGet("/v1/moderation/automation")]
    public async Task<IHttpActionResult> GetModerationAutomation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var bans    = Database.GetCollection<MothershipBanDbObject>(true)!.FindAll().ToList();
        var mutes   = Database.GetCollection<MothershipMuteDbObject>(true)!.FindAll().ToList();
        var reports = Database.GetCollection<MothershipReportDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { bans, mutes, reports });
    }

    /// <summary>Creates a mute or other moderation action (automation).</summary>
    [HttpPost("/v1/moderation/automation")]
    public async Task<IHttpActionResult> PostModerationAutomation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var action = Str(body, "action");

        if (action == "mute")
        {
            var mute = new MothershipMuteDbObject
            {
                MuteId          = NewId(),
                PlayerId        = Str(body, "player_id"),
                DurationMinutes = Int(body, "duration_minutes"),
                CreatedAt       = DateTime.UtcNow
            };
            Database.GetCollection<MothershipMuteDbObject>(true)!.Insert(mute);
            return Json(mute);
        }

        return JsonAnon(new { error = "Invalid action" }, HttpStatusCode.BadRequest);
    }

    /// <summary>Removes a mute by ID (automation).</summary>
    [HttpDelete("/v1/moderation/automation")]
    public async Task<IHttpActionResult> DeleteModerationAutomation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var muteId = request.GetQueryParameter("mute_id");
        if (muteId == null) return JsonAnon(new { error = "Mute not found" }, HttpStatusCode.NotFound);

        var col  = Database.GetCollection<MothershipMuteDbObject>(true)!;
        var mute = col.FindOne(m => m.MuteId == muteId);
        if (mute == null) return JsonAnon(new { error = "Mute not found" }, HttpStatusCode.NotFound);

        col.Delete(mute.Id);
        return JsonAnon(new { status = "deleted" });
    }

    // ── SERVER SHARED GROUP ───────────────────────────────────────────────────

    /// <summary>Creates a shared group (server).</summary>
    [HttpPost("/v1/server/shared-group")]
    public async Task<IHttpActionResult> ServerSharedGroup(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var action  = Str(body, "action");
        var groupId = body.TryGetValue("sharedGroupId", out var gid) && gid.ValueKind == JsonValueKind.String
            ? gid.GetString() ?? NewId() : NewId();

        if (action == "create" || string.IsNullOrEmpty(action))
        {
            var group = new MothershipSharedGroupDbObject
            {
                SharedGroupId = groupId,
                Data          = new(),
                Members       = new()
            };
            Database.GetCollection<MothershipSharedGroupDbObject>(true)!.Insert(group);
            return JsonAnon(new { SharedGroupId = groupId });
        }

        return JsonAnon(new { error = "Invalid action" }, HttpStatusCode.BadRequest);
    }

    // ── GENERIC SHARED GROUP ROUTES ───────────────────────────────────────────
    // Also act as fallback delegates for /v1/moderation/automation if route scanning
    // resolves these before the specific moderation handlers above.

    /// <summary>Returns a shared group or delegates to moderation automation.</summary>
    [HttpGet("/v1/{platform}/{group_id}")]
    public async Task<IHttpActionResult> GetSharedGroup(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform == "moderation" && group_id == "automation")
            return await GetModerationAutomation(request, response);

        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var group = Database.GetCollection<MothershipSharedGroupDbObject>(true)!.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);
        return Json(group);
    }

    /// <summary>Updates data in a shared group or delegates to moderation automation.</summary>
    [HttpPost("/v1/{platform}/{group_id}")]
    public async Task<IHttpActionResult> UpdateSharedGroup(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform == "moderation" && group_id == "automation")
            return await PostModerationAutomation(request, response);

        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var body = ParseBody(request);
        var col  = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        if (body.TryGetValue("data", out var dataEl) && dataEl.ValueKind == JsonValueKind.Object)
            foreach (var kv in dataEl.EnumerateObject())
                group.Data[kv.Name] = kv.Value.ToString();

        if (body.TryGetValue("keysToRemove", out var removeEl) && removeEl.ValueKind == JsonValueKind.Array)
            foreach (var key in removeEl.EnumerateArray())
                group.Data.Remove(key.GetString() ?? "");

        col.Update(group);
        return Json(group);
    }

    /// <summary>Deletes a shared group or delegates to moderation automation.</summary>
    [HttpDelete("/v1/{platform}/{group_id}")]
    public async Task<IHttpActionResult> DeleteSharedGroup(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform == "moderation" && group_id == "automation")
            return await DeleteModerationAutomation(request, response);

        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var col   = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        col.Delete(group.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Returns the data dictionary of a shared group.</summary>
    [HttpGet("/v1/{platform}/{group_id}/data")]
    public async Task<IHttpActionResult> GetSharedGroupData(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var group = Database.GetCollection<MothershipSharedGroupDbObject>(true)!.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);
        return Json(group);
    }

    /// <summary>Adds members to a shared group.</summary>
    [HttpPost("/v1/{platform}/{group_id}/members")]
    public async Task<IHttpActionResult> AddSharedGroupMembers(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var body  = ParseBody(request);
        var col   = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        group.Members.AddRange(StrList(body, "members"));
        col.Update(group);
        return Json(group);
    }

    /// <summary>Removes members from a shared group.</summary>
    [HttpDelete("/v1/{platform}/{group_id}/members")]
    public async Task<IHttpActionResult> RemoveSharedGroupMembers(IHttpRequest request, IHttpResponse response, string platform, string group_id)
    {
        if (platform != "shared-group") return JsonAnon(new { error = "Invalid platform" }, HttpStatusCode.BadRequest);

        var body    = ParseBody(request);
        var col     = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group   = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        var toRemove = StrList(body, "members");
        group.Members.RemoveAll(m => toRemove.Contains(m));
        col.Update(group);
        return Json(group);
    }

    // ── SERVER AUTH — BULK LOOKUPS ────────────────────────────────────────────

    /// <summary>Looks up multiple players by their Mothership player IDs.</summary>
    [HttpPost("/v1/server/player/auth/bulk_player_lookup")]
    public async Task<IHttpActionResult> BulkPlayerLookup(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var col     = Database.GetCollection<MothershipPlayerDbObject>(true)!;
        var results = new List<MothershipPlayerDbObject>();

        if (body.TryGetValue("Lookups", out var lookupsEl) && lookupsEl.ValueKind == JsonValueKind.Array)
        {
            foreach (var lookup in lookupsEl.EnumerateArray())
            {
                var pid = lookup.TryGetProperty("PlayerId", out var p) ? p.GetString() ?? "" : "";
                if (!string.IsNullOrEmpty(pid))
                {
                    var player = col.FindOne(pl => pl.MothershipPlayerId == pid);
                    if (player != null) results.Add(player);
                }
            }
        }

        return JsonAnon(new { Results = results });
    }

    /// <summary>Returns all account links.</summary>
    [HttpPost("/v1/server/player/auth/bulk_link_lookup")]
    public async Task<IHttpActionResult> BulkLinkLookup(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipLinkDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── TITLE AUTOMATION ─────────────────────────────────────────────────────

    /// <summary>Creates a new title (automation).</summary>
    [HttpPost("/v1/title/automation")]
    public async Task<IHttpActionResult> AutomationCreateTitle(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body  = ParseBody(request);
        var title = new MothershipTitleDbObject
        {
            TitleId   = Str(body, "title_id") is { Length: > 0 } s ? s : NewId(),
            TitleName = Str(body, "title_name")
        };
        Database.GetCollection<MothershipTitleDbObject>(true)!.Insert(title);
        return Json(title);
    }

    /// <summary>Returns titles, optionally filtered by title ID (automation).</summary>
    [HttpGet("/v1/title/automation")]
    public async Task<IHttpActionResult> AutomationGetTitles(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var titleId = request.GetQueryParameter("titleId");
        if (!string.IsNullOrEmpty(titleId))
        {
            var title = Database.GetCollection<MothershipTitleDbObject>(true)!.FindOne(t => t.TitleId == titleId);
            if (title != null) return Json(title);
        }

        var results = Database.GetCollection<MothershipTitleDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── ENVIRONMENT AUTOMATION ────────────────────────────────────────────────

    /// <summary>Creates a new environment for a title (automation).</summary>
    [HttpPost("/v1/env/automation")]
    public async Task<IHttpActionResult> AutomationCreateEnvironment(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var env  = new MothershipEnvironmentDbObject
        {
            EnvId        = Str(body, "envId") is { Length: > 0 } s ? s : NewId(),
            TitleId      = Str(body, "titleId"),
            EnvName      = Str(body, "envName"),
            RequiredTags = StrList(body, "requiredTags")
        };
        Database.GetCollection<MothershipEnvironmentDbObject>(true)!.Insert(env);
        return JsonAnon(new { env });
    }

    /// <summary>Returns environments, optionally filtered by env or title ID (automation).</summary>
    [HttpGet("/v1/env/automation")]
    public async Task<IHttpActionResult> AutomationGetEnvironments(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var envId   = request.GetQueryParameter("envId");
        var titleId = request.GetQueryParameter("titleId");
        var col     = Database.GetCollection<MothershipEnvironmentDbObject>(true)!;

        if (!string.IsNullOrEmpty(envId))
        {
            var env = col.FindOne(e => e.EnvId == envId);
            if (env != null) return Json(env);
        }
        else if (!string.IsNullOrEmpty(titleId))
        {
            var results = col.Find(e => e.TitleId == titleId).ToList();
            return JsonAnon(new { Results = results });
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    // ── DEPLOYMENT AUTOMATION ─────────────────────────────────────────────────

    /// <summary>Creates a new deployment (automation).</summary>
    [HttpPost("/v1/deployment/automation")]
    public async Task<IHttpActionResult> AutomationCreateDeployment(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body       = ParseBody(request);
        var deployment = new MothershipDeploymentDbObject
        {
            DeploymentId = Str(body, "deployment_id") is { Length: > 0 } s ? s : NewId(),
            TitleId      = Str(body, "title_id"),
            EnvId        = Str(body, "env_id"),
            Name         = Str(body, "name"),
            RequiredTags = StrList(body, "requiredTags")
        };
        Database.GetCollection<MothershipDeploymentDbObject>(true)!.Insert(deployment);
        return JsonAnon(new { deployment });
    }

    /// <summary>Returns deployments, optionally filtered by deployment ID (automation).</summary>
    [HttpGet("/v1/deployment/automation")]
    public async Task<IHttpActionResult> AutomationGetDeployments(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var deploymentId = request.GetQueryParameter("deployment_id");
        var col          = Database.GetCollection<MothershipDeploymentDbObject>(true)!;

        if (!string.IsNullOrEmpty(deploymentId))
        {
            var dep = col.FindOne(d => d.DeploymentId == deploymentId);
            if (dep != null) return Json(dep);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    // ── USER DATA ─────────────────────────────────────────────────────────────

    /// <summary>Creates or updates a user data key-value pair.</summary>
    [HttpPost("/v1/userdata")]
    public async Task<IHttpActionResult> PostUserData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var userId  = Str(body, "user_id");
        var keyName = Str(body, "key_name");
        var col     = Database.GetCollection<MothershipUserDataDbObject>(true)!;
        var existing = col.FindOne(d => d.UserId == userId && d.KeyName == keyName);

        if (existing != null)
        {
            existing.Value      = Str(body, "value");
            existing.Generation = existing.Generation + 1;
            col.Update(existing);
            return Json(existing);
        }

        var data = new MothershipUserDataDbObject
        {
            DataId     = NewId(),
            UserId     = userId,
            KeyName    = keyName,
            Value      = Str(body, "value"),
            Generation = 1
        };
        col.Insert(data);
        return Json(data);
    }

    /// <summary>Returns user data for a player, optionally filtered by key name.</summary>
    [HttpGet("/v1/userdata")]
    public async Task<IHttpActionResult> GetUserData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var userId  = request.GetQueryParameter("user_id") ?? "";
        var keyName = request.GetQueryParameter("key_name") ?? "";
        var col     = Database.GetCollection<MothershipUserDataDbObject>(true)!;

        if (!string.IsNullOrEmpty(userId) && !string.IsNullOrEmpty(keyName))
        {
            var item = col.FindOne(d => d.UserId == userId && d.KeyName == keyName);
            if (item == null) return JsonAnon(new { error = "User data not found" }, HttpStatusCode.NotFound);
            return Json(item);
        }

        if (!string.IsNullOrEmpty(userId))
        {
            var results = col.Find(d => d.UserId == userId).ToList();
            return JsonAnon(new { Results = results });
        }

        return JsonAnon(new { error = "Missing parameters" }, HttpStatusCode.BadRequest);
    }

    /// <summary>Deletes a specific user data key for a player.</summary>
    [HttpDelete("/v1/userdata")]
    public async Task<IHttpActionResult> DeleteUserData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var userId  = request.GetQueryParameter("user_id") ?? "";
        var keyName = request.GetQueryParameter("key_name") ?? "";
        var col     = Database.GetCollection<MothershipUserDataDbObject>(true)!;
        var item    = col.FindOne(d => d.UserId == userId && d.KeyName == keyName);
        if (item == null) return JsonAnon(new { error = "User data not found" }, HttpStatusCode.NotFound);

        col.Delete(item.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Creates metadata definition for a user data key (automation).</summary>
    [HttpPost("/v1/userdata/metadata/automation")]
    public async Task<IHttpActionResult> CreateUserDataMetadata(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body     = ParseBody(request);
        var metadata = new MothershipUserDataMetadataDbObject
        {
            MetadataId     = NewId(),
            TitleId        = Str(body, "title_id"),
            EnvId          = Str(body, "env_id"),
            KeyName        = Str(body, "key_name"),
            KeyPermissions = Str(body, "key_permissions"),
            PrivacyNotes   = Str(body, "privacy_notes")
        };
        Database.GetCollection<MothershipUserDataMetadataDbObject>(true)!.Insert(metadata);
        return Json(metadata);
    }

    /// <summary>Lists all user data metadata definitions (automation).</summary>
    [HttpGet("/v1/userdata/metadata/automation/list")]
    public async Task<IHttpActionResult> ListUserDataMetadata(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipUserDataMetadataDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── TITLE DATA ────────────────────────────────────────────────────────────

    /// <summary>Creates or updates a title data key-value pair.</summary>
    [HttpPost("/v1/title-data")]
    public async Task<IHttpActionResult> PostTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var key  = Str(body, "key");
        var col  = Database.GetCollection<MothershipTitleDataDbObject>(true)!;
        var item = col.FindOne(d => d.Key == key);

        if (item != null)
        {
            item.Value     = Str(body, "value");
            item.UpdatedAt = DateTime.UtcNow.ToString("O");
            col.Update(item);
            return Json(item);
        }

        var newItem = new MothershipTitleDataDbObject
        {
            Key       = key,
            Value     = Str(body, "value"),
            UpdatedAt = DateTime.UtcNow.ToString("O")
        };
        col.Insert(newItem);
        return Json(newItem);
    }

    /// <summary>Returns title data, optionally filtered by key.</summary>
    [HttpGet("/v1/title-data")]
    public async Task<IHttpActionResult> GetTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var key = request.GetQueryParameter("key");
        var col = Database.GetCollection<MothershipTitleDataDbObject>(true)!;

        if (!string.IsNullOrEmpty(key))
        {
            var item = col.FindOne(d => d.Key == key);
            if (item != null) return Json(item);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Deletes a title data entry by key.</summary>
    [HttpDelete("/v1/title-data")]
    public async Task<IHttpActionResult> DeleteTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var key  = request.GetQueryParameter("key") ?? "";
        var col  = Database.GetCollection<MothershipTitleDataDbObject>(true)!;
        var item = col.FindOne(d => d.Key == key);
        if (item == null) return JsonAnon(new { error = "Title data not found" }, HttpStatusCode.NotFound);

        col.Delete(item.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Returns all title data (no auth required, client-facing).</summary>
    [HttpGet("/v1/title-data/client")]
    public async Task<IHttpActionResult> ClientGetTitleData(IHttpRequest request, IHttpResponse response)
    {
        var results = Database.GetCollection<MothershipTitleDataDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Creates or updates a title data entry (automation).</summary>
    [HttpPost("/v1/title-data/automation")]
    public async Task<IHttpActionResult> AutomationSetTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var key  = Str(body, "key");
        var col  = Database.GetCollection<MothershipTitleDataDbObject>(true)!;
        var item = col.FindOne(d => d.Key == key);

        if (item != null)
        {
            item.Value     = Str(body, "value");
            item.UpdatedAt = DateTime.UtcNow.ToString("O");
            col.Update(item);
            return Json(item);
        }

        var newItem = new MothershipTitleDataDbObject
        {
            Key       = key,
            Value     = Str(body, "value"),
            UpdatedAt = DateTime.UtcNow.ToString("O")
        };
        col.Insert(newItem);
        return Json(newItem);
    }

    /// <summary>Returns all title data (automation).</summary>
    [HttpGet("/v1/title-data/automation")]
    public async Task<IHttpActionResult> AutomationGetTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipTitleDataDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a title data entry by key (automation).</summary>
    [HttpDelete("/v1/title-data/automation")]
    public async Task<IHttpActionResult> AutomationDeleteTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var key  = request.GetQueryParameter("key") ?? "";
        var col  = Database.GetCollection<MothershipTitleDataDbObject>(true)!;
        var item = col.FindOne(d => d.Key == key);
        if (item == null) return JsonAnon(new { error = "Title data not found" }, HttpStatusCode.NotFound);

        col.Delete(item.Id);
        return JsonAnon(new { status = "deleted" });
    }

    // ── OFFERS ────────────────────────────────────────────────────────────────

    /// <summary>Creates a new store offer (automation).</summary>
    [HttpPost("/v1/offers/automation")]
    public async Task<IHttpActionResult> AutomationCreateOffer(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body  = ParseBody(request);
        var offer = new MothershipOfferDbObject
        {
            OfferId          = NewId(),
            Name             = Str(body, "name"),
            TitleId          = Str(body, "titleId"),
            EnvId            = Str(body, "envId"),
            TransactionId    = Str(body, "transaction_id"),
            BundlePricingJson = body.TryGetValue("bundle_pricing", out var bp) ? bp.ToString() : "{}",
            DiscountPercent  = Int(body, "discount_percent")
        };
        Database.GetCollection<MothershipOfferDbObject>(true)!.Insert(offer);
        return Json(offer);
    }

    /// <summary>Returns all store offers (automation).</summary>
    [HttpGet("/v1/offers/automation")]
    public async Task<IHttpActionResult> AutomationGetOffers(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Creates a binding between an offer and a deployment (automation).</summary>
    [HttpPost("/v1/offerbindings/automation")]
    public async Task<IHttpActionResult> AutomationCreateOfferBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var binding = new MothershipOfferBindingDbObject
        {
            OfferBindingId = NewId(),
            TitleId        = Str(body, "title_id"),
            EnvId          = Str(body, "env_id"),
            DeploymentId   = Str(body, "deployment_id"),
            OfferDisplayId = Str(body, "offer_display_id"),
            OfferId        = Str(body, "offer_id"),
            Committed      = Bool(body, "committed"),
            DisplayIndex   = Int(body, "display_index")
        };
        Database.GetCollection<MothershipOfferBindingDbObject>(true)!.Insert(binding);
        return Json(binding);
    }

    /// <summary>Returns all offer bindings (automation).</summary>
    [HttpGet("/v1/offerbindings/automation")]
    public async Task<IHttpActionResult> AutomationGetOfferBindings(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferBindingDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // Note: Python had a trailing-slash variant of this route used for committing a binding
    /// <summary>Sets the committed flag on an offer binding.</summary>
    [HttpPost("/v1/offerbindings/automation/commit")]
    public async Task<IHttpActionResult> AutomationCommitOfferBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body      = ParseBody(request);
        var bindingId = Str(body, "offer_binding_id");
        var col       = Database.GetCollection<MothershipOfferBindingDbObject>(true)!;
        var binding   = col.FindOne(b => b.OfferBindingId == bindingId);
        if (binding == null) return JsonAnon(new { error = "Binding not found" }, HttpStatusCode.NotFound);

        binding.Committed = Bool(body, "committed", true);
        col.Update(binding);
        return Json(binding);
    }

    /// <summary>Creates an offer display entry (automation).</summary>
    [HttpPost("/v1/offerdisplays/automation")]
    public async Task<IHttpActionResult> AutomationCreateOfferDisplay(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var display = new MothershipOfferDisplayDbObject
        {
            OfferDisplayId = NewId(),
            Name           = Str(body, "name"),
            Description    = Str(body, "description")
        };
        Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.Insert(display);
        return Json(display);
    }

    /// <summary>Returns all offer displays (automation).</summary>
    [HttpGet("/v1/offerdisplays/automation")]
    public async Task<IHttpActionResult> AutomationGetOfferDisplays(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── PURCHASE ──────────────────────────────────────────────────────────────

    /// <summary>Processes a client purchase by offer ID.</summary>
    [HttpPost("/v1/purchase/client")]
    public async Task<IHttpActionResult> ClientPurchase(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var offerId = Str(body, "OfferId");
        var offer   = Database.GetCollection<MothershipOfferDbObject>(true)!.FindOne(o => o.OfferId == offerId);
        if (offer == null) return JsonAnon(new { error = "Offer not found" }, HttpStatusCode.NotFound);

        return JsonAnon(new { Changes = new { purchased = true, offer_id = offerId, timestamp = DateTime.UtcNow.ToString("O") } });
    }

    /// <summary>Refreshes in-app purchase state for the client.</summary>
    [HttpPost("/v1/purchase/client/refresh-iap")]
    public async Task<IHttpActionResult> ClientRefreshIap(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { status = "refreshed", timestamp = DateTime.UtcNow.ToString("O") });
    }

    /// <summary>Processes a server-side purchase and returns a transaction ID.</summary>
    [HttpPost("/v1/purchase")]
    public async Task<IHttpActionResult> ServerPurchase(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var transactionId = NewId();
        return JsonAnon(new
        {
            Results = new[] { new { transaction_id = transactionId, status = "completed", timestamp = DateTime.UtcNow.ToString("O") } }
        });
    }

    // ── ENTITLEMENT CATALOG ───────────────────────────────────────────────────

    /// <summary>Creates an entitlement catalog entry (automation).</summary>
    [HttpPost("/v1/entitlement-catalog/automation")]
    public async Task<IHttpActionResult> AutomationCreateEntitlement(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body        = ParseBody(request);
        var entitlement = new MothershipEntitlementDbObject
        {
            EntitlementId = NewId(),
            Name          = Str(body, "name"),
            Type          = Str(body, "type") is { Length: > 0 } t ? t : "DURABLE",
            ItemClass     = Str(body, "item_class")
        };
        Database.GetCollection<MothershipEntitlementDbObject>(true)!.Insert(entitlement);
        return Json(entitlement);
    }

    /// <summary>Returns entitlements, optionally filtered by ID (automation).</summary>
    [HttpGet("/v1/entitlement-catalog/automation")]
    public async Task<IHttpActionResult> AutomationGetEntitlements(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var entitlementId = request.GetQueryParameter("entitlement_id");
        var col           = Database.GetCollection<MothershipEntitlementDbObject>(true)!;

        if (!string.IsNullOrEmpty(entitlementId))
        {
            var item = col.FindOne(e => e.EntitlementId == entitlementId);
            if (item != null) return Json(item);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Updates an existing entitlement entry (automation).</summary>
    [HttpPut("/v1/entitlement-catalog/automation")]
    public async Task<IHttpActionResult> AutomationUpdateEntitlement(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body          = ParseBody(request);
        var entitlementId = Str(body, "entitlement_id");
        var col           = Database.GetCollection<MothershipEntitlementDbObject>(true)!;
        var item          = col.FindOne(e => e.EntitlementId == entitlementId);
        if (item == null) return JsonAnon(new { error = "Entitlement not found" }, HttpStatusCode.NotFound);

        if (Str(body, "name")       is { Length: > 0 } n) item.Name      = n;
        if (Str(body, "type")       is { Length: > 0 } t) item.Type      = t;
        if (Str(body, "item_class") is { Length: > 0 } c) item.ItemClass = c;
        col.Update(item);
        return JsonAnon(new { result = item });
    }

    // ── TRANSACTION CATALOG ───────────────────────────────────────────────────

    /// <summary>Creates a transaction catalog entry (automation).</summary>
    [HttpPost("/v1/transaction-catalog/automation")]
    public async Task<IHttpActionResult> AutomationCreateTransaction(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body        = ParseBody(request);
        var transaction = new MothershipTransactionCatalogDbObject
        {
            TransactionId  = NewId(),
            Name           = Str(body, "name"),
            OperationsJson = body.TryGetValue("operations", out var ops) ? ops.ToString() : "[]"
        };
        Database.GetCollection<MothershipTransactionCatalogDbObject>(true)!.Insert(transaction);
        return Json(transaction);
    }

    /// <summary>Returns transaction catalog entries, optionally filtered by ID (automation).</summary>
    [HttpGet("/v1/transaction-catalog/automation")]
    public async Task<IHttpActionResult> AutomationGetTransactions(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var transactionId = request.GetQueryParameter("transaction_id");
        var col           = Database.GetCollection<MothershipTransactionCatalogDbObject>(true)!;

        if (!string.IsNullOrEmpty(transactionId))
        {
            var item = col.FindOne(t => t.TransactionId == transactionId);
            if (item != null) return Json(item);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Returns an empty sunset list for transaction catalog (automation).</summary>
    [HttpGet("/v1/transaction-catalog/automation/sunset")]
    public async Task<IHttpActionResult> AutomationTransactionSunset(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { Results = Array.Empty<object>() });
    }

    // ── INVENTORY ─────────────────────────────────────────────────────────────

    /// <summary>Returns inventory items for a player.</summary>
    [HttpGet("/v1/inventory")]
    public async Task<IHttpActionResult> GetInventory(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var playerId = request.GetQueryParameter("player_id") ?? "";
        var items    = Database.GetCollection<MothershipInventoryItemDbObject>(true)!
            .Find(i => i.PlayerId == playerId).ToList();
        return JsonAnon(new { Results = items });
    }

    /// <summary>Adds or updates an inventory item for a player.</summary>
    [HttpPost("/v1/inventory")]
    public async Task<IHttpActionResult> AddInventoryItem(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body     = ParseBody(request);
        var playerId = Str(body, "player_id");
        var itemId   = Str(body, "item_id") is { Length: > 0 } s ? s : NewId();
        var col      = Database.GetCollection<MothershipInventoryItemDbObject>(true)!;

        var existing = col.FindOne(i => i.PlayerId == playerId && i.ItemId == itemId);
        if (existing != null)
        {
            existing.ItemDataJson = JsonSerializer.Serialize(body);
            col.Update(existing);
        }
        else
        {
            col.Insert(new MothershipInventoryItemDbObject
            {
                PlayerId     = playerId,
                ItemId       = itemId,
                ItemDataJson = JsonSerializer.Serialize(body)
            });
        }

        var allItems = col.Find(i => i.PlayerId == playerId).ToList();
        return JsonAnon(new { Results = allItems });
    }

    // ── STOREFRONT ────────────────────────────────────────────────────────────

    /// <summary>Returns the storefront with offers and displays for a client.</summary>
    [HttpGet("/v1/storefront/client")]
    public async Task<IHttpActionResult> ClientGetStorefront(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var offers   = Database.GetCollection<MothershipOfferDbObject>(true)!.FindAll().ToList();
        var displays = Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = new { offers, featured = Array.Empty<object>(), displays } });
    }

    // ── PROGRESSION TRACKS ────────────────────────────────────────────────────

    /// <summary>Creates a new progression track.</summary>
    [HttpPost("/v1/progression")]
    public async Task<IHttpActionResult> CreateProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body  = ParseBody(request);
        var track = new MothershipProgressionTrackDbObject
        {
            TrackId      = NewId(),
            Name         = Str(body, "name"),
            LevelsJson   = body.TryGetValue("levels",   out var lv) ? lv.ToString() : "[]",
            TriggersJson = body.TryGetValue("triggers", out var tr) ? tr.ToString() : "[]"
        };
        Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.Insert(track);
        return Json(track);
    }

    /// <summary>Returns progression tracks, optionally filtered by track ID.</summary>
    [HttpGet("/v1/progression")]
    public async Task<IHttpActionResult> GetProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var trackId = request.GetQueryParameter("track_id");
        var col     = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!;

        if (!string.IsNullOrEmpty(trackId))
        {
            var track = col.FindOne(t => t.TrackId == trackId);
            if (track != null) return Json(track);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Deletes a progression track by ID.</summary>
    [HttpDelete("/v1/progression")]
    public async Task<IHttpActionResult> DeleteProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var trackId = request.GetQueryParameter("track_id") ?? "";
        var col     = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!;
        var track   = col.FindOne(t => t.TrackId == trackId);
        if (track == null) return JsonAnon(new { error = "Track not found" }, HttpStatusCode.NotFound);

        col.Delete(track.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Creates a progression track, level or trigger (automation).</summary>
    [HttpPost("/v1/progression/automation")]
    public async Task<IHttpActionResult> AutomationCreateProgressionTrack(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var action = Str(body, "action");

        if (action == "create_level")   return JsonAnon(new { level_id   = NewId() });
        if (action == "create_trigger") return JsonAnon(new { trigger_id = NewId() });

        var track = new MothershipProgressionTrackDbObject
        {
            TrackId      = NewId(),
            Name         = Str(body, "name"),
            LevelsJson   = body.TryGetValue("levels",   out var lv) ? lv.ToString() : "[]",
            TriggersJson = body.TryGetValue("triggers", out var tr) ? tr.ToString() : "[]"
        };
        Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.Insert(track);
        return Json(track);
    }

    /// <summary>Returns all progression tracks (automation).</summary>
    [HttpGet("/v1/progression/automation")]
    public async Task<IHttpActionResult> AutomationGetProgressionTracks(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a progression track by ID (automation).</summary>
    [HttpDelete("/v1/progression/automation")]
    public async Task<IHttpActionResult> AutomationDeleteProgressionTrack(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var trackId = request.GetQueryParameter("track_id") ?? "";
        var col     = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!;
        var track   = col.FindOne(t => t.TrackId == trackId);
        if (track == null) return JsonAnon(new { error = "Track not found" }, HttpStatusCode.NotFound);

        col.Delete(track.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Binds a progression track to a deployment (automation).</summary>
    [HttpPost("/v1/progression/automation/bindings")]
    public async Task<IHttpActionResult> AutomationCreateProgressionBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var binding = new MothershipProgressionTrackBindingDbObject
        {
            BindingId    = NewId(),
            TrackId      = Str(body, "track_id"),
            DeploymentId = Str(body, "deployment_id")
        };
        Database.GetCollection<MothershipProgressionTrackBindingDbObject>(true)!.Insert(binding);
        return Json(binding);
    }

    /// <summary>Returns all progression track bindings (automation).</summary>
    [HttpGet("/v1/progression/automation/bindings")]
    public async Task<IHttpActionResult> AutomationGetProgressionBindings(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTrackBindingDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Records additional progress for a player on a track (server).</summary>
    [HttpPost("/v1/progression/server")]
    public async Task<IHttpActionResult> ServerProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body               = ParseBody(request);
        var playerId           = Str(body, "PlayerId");
        var trackId            = Str(body, "TrackId");
        var additionalProgress = Int(body, "AdditionalProgress");
        return JsonAnon(new
        {
            player_id  = playerId,
            track_id   = trackId,
            progress   = additionalProgress,
            updated_at = DateTime.UtcNow.ToString("O")
        });
    }

    // ── PROGRESSION TREES ─────────────────────────────────────────────────────

    /// <summary>Creates a new progression tree.</summary>
    [HttpPost("/v1/progression-tree")]
    public async Task<IHttpActionResult> CreateProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body = ParseBody(request);
        var tree = new MothershipProgressionTreeDbObject
        {
            TreeId    = NewId(),
            Name      = Str(body, "name"),
            NodesJson = body.TryGetValue("nodes", out var nd) ? nd.ToString() : "[]"
        };
        Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.Insert(tree);
        return Json(tree);
    }

    /// <summary>Returns progression trees, optionally filtered by tree ID.</summary>
    [HttpGet("/v1/progression-tree")]
    public async Task<IHttpActionResult> GetProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var treeId = request.GetQueryParameter("tree_id");
        var col    = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!;

        if (!string.IsNullOrEmpty(treeId))
        {
            var tree = col.FindOne(t => t.TreeId == treeId);
            if (tree != null) return Json(tree);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Deletes a progression tree by ID.</summary>
    [HttpDelete("/v1/progression-tree")]
    public async Task<IHttpActionResult> DeleteProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var treeId = request.GetQueryParameter("tree_id") ?? "";
        var col    = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!;
        var tree   = col.FindOne(t => t.TreeId == treeId);
        if (tree == null) return JsonAnon(new { error = "Tree not found" }, HttpStatusCode.NotFound);

        col.Delete(tree.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Creates a progression tree, node or lock action (automation).</summary>
    [HttpPost("/v1/progression-tree/automation")]
    public async Task<IHttpActionResult> AutomationCreateProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var action = Str(body, "action");

        if (action == "create_node") return JsonAnon(new { node_id = NewId() });
        if (action == "lock_node")   return JsonAnon(new { status  = "locked" });

        var tree = new MothershipProgressionTreeDbObject
        {
            TreeId    = NewId(),
            Name      = Str(body, "name"),
            NodesJson = body.TryGetValue("nodes", out var nd) ? nd.ToString() : "[]"
        };
        Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.Insert(tree);
        return Json(tree);
    }

    /// <summary>Returns all progression trees (automation).</summary>
    [HttpGet("/v1/progression-tree/automation")]
    public async Task<IHttpActionResult> AutomationGetProgressionTrees(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a progression tree by ID (automation).</summary>
    [HttpDelete("/v1/progression-tree/automation")]
    public async Task<IHttpActionResult> AutomationDeleteProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var treeId = request.GetQueryParameter("tree_id") ?? "";
        var col    = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!;
        var tree   = col.FindOne(t => t.TreeId == treeId);
        if (tree == null) return JsonAnon(new { error = "Tree not found" }, HttpStatusCode.NotFound);

        col.Delete(tree.Id);
        return JsonAnon(new { status = "deleted" });
    }

    /// <summary>Binds a progression tree to a deployment (automation).</summary>
    [HttpPost("/v1/progression-tree/automation/bindings")]
    public async Task<IHttpActionResult> AutomationCreateTreeBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body    = ParseBody(request);
        var binding = new MothershipProgressionTreeBindingDbObject
        {
            BindingId    = NewId(),
            TreeId       = Str(body, "tree_id"),
            DeploymentId = Str(body, "deployment_id")
        };
        Database.GetCollection<MothershipProgressionTreeBindingDbObject>(true)!.Insert(binding);
        return Json(binding);
    }

    /// <summary>Returns progression tree bindings, optionally filtered by ID (automation).</summary>
    [HttpGet("/v1/progression-tree/automation/bindings")]
    public async Task<IHttpActionResult> AutomationGetTreeBindings(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var bindingId = request.GetQueryParameter("binding_id");
        var col       = Database.GetCollection<MothershipProgressionTreeBindingDbObject>(true)!;

        if (!string.IsNullOrEmpty(bindingId))
        {
            var b = col.FindOne(x => x.BindingId == bindingId);
            if (b != null) return Json(b);
        }

        return JsonAnon(new { Results = col.FindAll().ToList() });
    }

    /// <summary>Deletes a progression tree binding by ID (automation).</summary>
    [HttpDelete("/v1/progression-tree/automation/bindings")]
    public async Task<IHttpActionResult> AutomationDeleteTreeBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var bindingId = request.GetQueryParameter("binding_id") ?? "";
        var col       = Database.GetCollection<MothershipProgressionTreeBindingDbObject>(true)!;
        var binding   = col.FindOne(b => b.BindingId == bindingId);
        if (binding == null) return JsonAnon(new { error = "Binding not found" }, HttpStatusCode.NotFound);

        col.Delete(binding.Id);
        return JsonAnon(new { status = "deleted" });
    }

    // ── ANALYTICS (platform batch) ────────────────────────────────────────────

    /// <summary>Receives and stores an analytics event batch for any platform.</summary>
    [HttpPost("/v1/{platform}/analytics/event/batch")]
    public async Task<IHttpActionResult> PlatformAnalyticsBatch(IHttpRequest request, IHttpResponse response, string platform)
    {
        var body   = ParseBody(request);
        var col    = Database.GetCollection<MothershipAnalyticsEventDbObject>(true)!;
        var events = 0;

        if (body.TryGetValue("events", out var eventsEl) && eventsEl.ValueKind == JsonValueKind.Array)
        {
            foreach (var evt in eventsEl.EnumerateArray())
            {
                col.Insert(new MothershipAnalyticsEventDbObject
                {
                    EventId   = NewId(),
                    Platform  = platform,
                    Timestamp = DateTime.UtcNow.ToString("O"),
                    DataJson  = evt.ToString()
                });
                events++;
            }
        }

        return JsonAnon(new { status = "success", events_processed = events, timestamp = DateTime.UtcNow.ToString("O") });
    }

    // ── PERMISSIONS ───────────────────────────────────────────────────────────

    /// <summary>Returns client permission and role definitions.</summary>
    [HttpGet("/v1/permissions/client")]
    public async Task<IHttpActionResult> ClientPermissions(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { permissions = new[] { "read", "write", "execute" }, roles = new[] { "player" } });
    }

    /// <summary>Returns server/admin permission and role definitions.</summary>
    [HttpGet("/v1/permissions/server")]
    public async Task<IHttpActionResult> ServerPermissions(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { permissions = new[] { "admin", "read", "write", "execute", "delete" }, roles = new[] { "server", "admin" } });
    }

    // ── SERVER API KEYS ───────────────────────────────────────────────────────

    /// <summary>Creates a new server API key (automation).</summary>
    [HttpPost("/v1/server/automation/api_key")]
    public async Task<IHttpActionResult> CreateApiKey(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body   = ParseBody(request);
        var keyObj = new MothershipApiKeyDbObject
        {
            KeyId     = NewId(),
            ApiKey    = NewId(),
            Name      = Str(body, "name"),
            CreatedAt = DateTime.UtcNow.ToString("O")
        };
        Database.GetCollection<MothershipApiKeyDbObject>(true)!.Insert(keyObj);
        return Json(keyObj);
    }

    /// <summary>Returns all server API keys (automation).</summary>
    [HttpGet("/v1/server/automation/api_key")]
    public async Task<IHttpActionResult> GetApiKeys(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipApiKeyDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── NOTIFICATIONS ─────────────────────────────────────────────────────────

    /// <summary>Sends a notification to a player (server).</summary>
    [HttpPost("/v1/notifications/server/send")]
    public async Task<IHttpActionResult> ServerSendNotification(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body         = ParseBody(request);
        var notification = new MothershipNotificationDbObject
        {
            NotificationId = NewId(),
            PlayerId       = Str(body, "player_id"),
            Message        = Str(body, "message"),
            Type           = Str(body, "type"),
            SentAt         = DateTime.UtcNow.ToString("O")
        };
        Database.GetCollection<MothershipNotificationDbObject>(true)!.Insert(notification);
        return Json(notification);
    }

    // ── HEALTH & INFO ─────────────────────────────────────────────────────────

    /// <summary>Returns basic service info (name, version, status).</summary>
    [HttpGet("/")]
    public async Task<IHttpActionResult> Index(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new
        {
            service     = "Mothership API",
            version     = "1.0.0",
            status      = "running",
            sdk_version = "v2025.11.24.1"
        });
    }

    /// <summary>Returns the health status and current timestamp.</summary>
    [HttpGet("/health")]
    public async Task<IHttpActionResult> Health(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new { status = "healthy", timestamp = DateTime.UtcNow.ToString("O") });
    }

    /// <summary>Returns item counts across all storage collections.</summary>
    [HttpGet("/debug/storage")]
    public async Task<IHttpActionResult> DebugStorage(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new
        {
            players               = Database.GetCollection<MothershipPlayerDbObject>(true)!.Count(),
            sessions              = Database.GetCollection<MothershipSessionDbObject>(true)!.Count(),
            bans                  = Database.GetCollection<MothershipBanDbObject>(true)!.Count(),
            mutes                 = Database.GetCollection<MothershipMuteDbObject>(true)!.Count(),
            reports               = Database.GetCollection<MothershipReportDbObject>(true)!.Count(),
            shared_groups         = Database.GetCollection<MothershipSharedGroupDbObject>(true)!.Count(),
            titles                = Database.GetCollection<MothershipTitleDbObject>(true)!.Count(),
            environments          = Database.GetCollection<MothershipEnvironmentDbObject>(true)!.Count(),
            deployments           = Database.GetCollection<MothershipDeploymentDbObject>(true)!.Count(),
            user_data             = Database.GetCollection<MothershipUserDataDbObject>(true)!.Count(),
            title_data            = Database.GetCollection<MothershipTitleDataDbObject>(true)!.Count(),
            offers                = Database.GetCollection<MothershipOfferDbObject>(true)!.Count(),
            offer_bindings        = Database.GetCollection<MothershipOfferBindingDbObject>(true)!.Count(),
            offer_displays        = Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.Count(),
            entitlements          = Database.GetCollection<MothershipEntitlementDbObject>(true)!.Count(),
            transactions          = Database.GetCollection<MothershipTransactionCatalogDbObject>(true)!.Count(),
            inventory             = Database.GetCollection<MothershipInventoryItemDbObject>(true)!.Count(),
            progression_tracks    = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.Count(),
            progression_trees     = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.Count(),
            api_keys              = Database.GetCollection<MothershipApiKeyDbObject>(true)!.Count(),
            notifications         = Database.GetCollection<MothershipNotificationDbObject>(true)!.Count(),
            analytics_events      = Database.GetCollection<MothershipAnalyticsEventDbObject>(true)!.Count()
        });
    }

    // ── ADMIN STORAGE ─────────────────────────────────────────────────────────

    /// <summary>Returns all items in a named storage collection (admin).</summary>
    [HttpGet("/storage/{key}")]
    public async Task<IHttpActionResult> GetStorage(IHttpRequest request, IHttpResponse response, string key)
    {
        object? data = key switch
        {
            "players"            => (object)Database.GetCollection<MothershipPlayerDbObject>(true)!.FindAll().ToList(),
            "sessions"           => Database.GetCollection<MothershipSessionDbObject>(true)!.FindAll().ToList(),
            "account_associations" => Database.GetCollection<MothershipAssociationDbObject>(true)!.FindAll().ToList(),
            "account_links"      => Database.GetCollection<MothershipLinkDbObject>(true)!.FindAll().ToList(),
            "bans"               => Database.GetCollection<MothershipBanDbObject>(true)!.FindAll().ToList(),
            "mutes"              => Database.GetCollection<MothershipMuteDbObject>(true)!.FindAll().ToList(),
            "reports"            => Database.GetCollection<MothershipReportDbObject>(true)!.FindAll().ToList(),
            "shared_groups"      => Database.GetCollection<MothershipSharedGroupDbObject>(true)!.FindAll().ToList(),
            "titles"             => Database.GetCollection<MothershipTitleDbObject>(true)!.FindAll().ToList(),
            "environments"       => Database.GetCollection<MothershipEnvironmentDbObject>(true)!.FindAll().ToList(),
            "deployments"        => Database.GetCollection<MothershipDeploymentDbObject>(true)!.FindAll().ToList(),
            "user_data"          => Database.GetCollection<MothershipUserDataDbObject>(true)!.FindAll().ToList(),
            "user_data_metadata" => Database.GetCollection<MothershipUserDataMetadataDbObject>(true)!.FindAll().ToList(),
            "title_data"         => Database.GetCollection<MothershipTitleDataDbObject>(true)!.FindAll().ToList(),
            "offers"             => Database.GetCollection<MothershipOfferDbObject>(true)!.FindAll().ToList(),
            "offer_bindings"     => Database.GetCollection<MothershipOfferBindingDbObject>(true)!.FindAll().ToList(),
            "offer_displays"     => Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.FindAll().ToList(),
            "entitlements"       => Database.GetCollection<MothershipEntitlementDbObject>(true)!.FindAll().ToList(),
            "transactions"       => Database.GetCollection<MothershipTransactionCatalogDbObject>(true)!.FindAll().ToList(),
            "inventory"          => Database.GetCollection<MothershipInventoryItemDbObject>(true)!.FindAll().ToList(),
            "progression_tracks" => Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.FindAll().ToList(),
            "progression_track_bindings" => Database.GetCollection<MothershipProgressionTrackBindingDbObject>(true)!.FindAll().ToList(),
            "progression_trees"  => Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.FindAll().ToList(),
            "progression_tree_bindings" => Database.GetCollection<MothershipProgressionTreeBindingDbObject>(true)!.FindAll().ToList(),
            "api_keys"           => Database.GetCollection<MothershipApiKeyDbObject>(true)!.FindAll().ToList(),
            "notifications"      => Database.GetCollection<MothershipNotificationDbObject>(true)!.FindAll().ToList(),
            "analytics_events"   => Database.GetCollection<MothershipAnalyticsEventDbObject>(true)!.FindAll().ToList(),
            "friends"            => Database.GetCollection<MothershipFriendEntryDbObject>(true)!.FindAll().ToList(),
            "privacy_states"     => Database.GetCollection<MothershipPrivacyStateDbObject>(true)!.FindAll().ToList(),
            _                    => null
        };

        if (data == null) return JsonAnon(new { error = "Invalid storage key" }, HttpStatusCode.BadRequest);
        return JsonAnon(data);
    }

    /// <summary>Creates a new item in a named storage collection (admin).</summary>
    [HttpPost("/storage/{key}")]
    public async Task<IHttpActionResult> CreateStorageItem(IHttpRequest request, IHttpResponse response, string key)
    {
        // Generic create — returns 201 with the posted body plus a generated id field
        var body = ParseBody(request);
        if (!body.TryGetValue("id", out _))
        {
            // We return the body as-is but with an injected id; since we can't mutate JsonElement,
            // we rebuild as anonymous object with id prepended
            return JsonAnon(new { id = NewId(), data = body }, HttpStatusCode.Created);
        }
        return JsonAnon(new { id = NewId(), data = body }, HttpStatusCode.Created);
    }

    // ── FRIENDS ───────────────────────────────────────────────────────────────

    /// <summary>Returns a player's friend list and privacy state.</summary>
    [HttpPost("/api/GetFriendsV2")]
    public async Task<IHttpActionResult> GetFriendsV2(IHttpRequest request, IHttpResponse response)
    {
        var body      = ParseBody(request);
        var playFabId = Str(body, "PlayFabId");
        if (string.IsNullOrEmpty(playFabId))
            return JsonAnon(new { StatusCode = 400, Error = "PlayFabId is required" }, HttpStatusCode.BadRequest);

        var friends     = Database.GetCollection<MothershipFriendEntryDbObject>(true)!
            .Find(f => f.OwnerId == playFabId).ToList();
        var privacyRow  = Database.GetCollection<MothershipPrivacyStateDbObject>(true)!
            .FindOne(p => p.PlayFabId == playFabId);
        var privacyState = privacyRow?.PrivacyState ?? 0;

        var friendList = friends.Select(f => new
        {
            Presence = new
            {
                FriendLinkId = f.FriendLinkId,
                UserName     = f.UserName,
                RoomId       = f.RoomId,
                Zone         = f.Zone,
                Region       = f.Region,
                IsPublic     = f.IsPublic
            },
            Created = f.Created
        }).ToList();

        return JsonAnon(new
        {
            Result = new { Friends = friendList, MyPrivacyState = privacyState },
            StatusCode = 200,
            Error = (string?)null
        });
    }

    /// <summary>Adds a friend entry for a player.</summary>
    [HttpPost("/api/RequestFriend")]
    public async Task<IHttpActionResult> RequestFriend(IHttpRequest request, IHttpResponse response)
    {
        var body               = ParseBody(request);
        var playFabId          = Str(body, "PlayFabId");
        var myFriendLinkId     = Str(body, "MyFriendLinkId");
        var friendFriendLinkId = Str(body, "FriendFriendLinkId");

        if (string.IsNullOrEmpty(playFabId) || string.IsNullOrEmpty(myFriendLinkId) || string.IsNullOrEmpty(friendFriendLinkId))
            return JsonAnon(new { error = "Missing required fields" }, HttpStatusCode.BadRequest);

        var col      = Database.GetCollection<MothershipFriendEntryDbObject>(true)!;
        var existing = col.FindOne(f => f.OwnerId == playFabId && f.FriendLinkId == friendFriendLinkId);
        if (existing != null)
            return JsonAnon(new { error = "Already friends" }, HttpStatusCode.Conflict);

        col.Insert(new MothershipFriendEntryDbObject
        {
            OwnerId      = playFabId,
            FriendLinkId = friendFriendLinkId,
            UserName     = $"Player_{(friendFriendLinkId.Length >= 8 ? friendFriendLinkId[..8] : friendFriendLinkId)}",
            RoomId       = "",
            Zone         = "",
            Region       = "",
            IsPublic     = true,
            Created      = DateTime.UtcNow.ToString("O")
        });

        return JsonAnon(new { success = true });
    }

    /// <summary>Removes a friend entry from a player's friend list.</summary>
    [HttpPost("/api/RemoveFriend")]
    public async Task<IHttpActionResult> RemoveFriend(IHttpRequest request, IHttpResponse response)
    {
        var body               = ParseBody(request);
        var playFabId          = Str(body, "PlayFabId");
        var friendFriendLinkId = Str(body, "FriendFriendLinkId");

        if (string.IsNullOrEmpty(playFabId) || string.IsNullOrEmpty(friendFriendLinkId))
            return JsonAnon(new { error = "Missing required fields" }, HttpStatusCode.BadRequest);

        var col    = Database.GetCollection<MothershipFriendEntryDbObject>(true)!;
        var friend = col.FindOne(f => f.OwnerId == playFabId && f.FriendLinkId == friendFriendLinkId);
        if (friend != null) col.Delete(friend.Id);

        return JsonAnon(new { success = true });
    }

    /// <summary>Sets the privacy visibility state for a player.</summary>
    [HttpPost("/api/SetPrivacyState")]
    public async Task<IHttpActionResult> SetPrivacyState(IHttpRequest request, IHttpResponse response)
    {
        var body             = ParseBody(request);
        var playFabId        = Str(body, "PlayFabId");
        var privacyStateStr  = Str(body, "PrivacyState");

        if (string.IsNullOrEmpty(playFabId))
            return JsonAnon(new { StatusCode = 400, Error = "PlayFabId is required" }, HttpStatusCode.BadRequest);

        var privacyState = privacyStateStr switch
        {
            "PUBLIC_ONLY" => 1,
            "HIDDEN"      => 2,
            _             => 0
        };

        var col      = Database.GetCollection<MothershipPrivacyStateDbObject>(true)!;
        var existing = col.FindOne(p => p.PlayFabId == playFabId);

        if (existing != null)
        {
            existing.PrivacyState = privacyState;
            col.Update(existing);
        }
        else
        {
            col.Insert(new MothershipPrivacyStateDbObject { PlayFabId = playFabId, PrivacyState = privacyState });
        }

        return JsonAnon(new { StatusCode = 200, Error = (string?)null });
    }
}
