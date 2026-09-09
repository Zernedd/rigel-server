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
    const string HOSTNAME      = "localhost";
    const ushort PORT          = 90;
    const string JWT_SECRET    = "53830568ce039c1326ea5f1c9e05e17a01c9b557969172d6c3b63fc72bdcc2cc87ee43cbe97e7cdf4ea3707264ac4c55ec91d4febd793f96c95947f8a1cb1450";
    const string ENVIRONMENT_ID = "7f3a99dd-5598-4725-98cf-6538d28feb9f";
    const string TENANT_ID     = "f3e9fb19";

    // --- Meta Quest Platform Integrity (attestation) verification -------------------------------
    // OUR repackaged app's credentials (plat ids swapped in the APK). Baked in intentionally: the
    // backend binary + source are private. Composed as "OC|{id}|{secret}" to authorize the
    // graph.oculus.com/platform_integrity/verify call. FILL THESE IN (kept out of chat on purpose).
    // Rigel app (org.zern.rigel) - used to verify Quest attestation + user-proof nonces against
    // Meta's graph API. This is the App ID the client's build is initialised with (MobileAppId in
    // the OBB), so attestation tokens are minted under it and must be verified with its secret.
    const string META_APP_ID     = "1358918123966018";
    const string META_APP_SECRET = "fd2e8b4be45e99a72ce8486a0ac3b8f1";
    // The RIFT (PC/Windows) build is a SEPARATE Meta app with its own creds. A UserProof nonce minted
    // by a client validates only under the app it came from, so nonce checks try both app cred sets
    // (see VerifyMetaUserNonce) — Quest players validate under the Quest app, Rift under the Rift app.
    const string RIFT_APP_ID     = "2240289680101933";
    const string RIFT_APP_SECRET = "a23fa419167728390ff2a0cebbcdae3b";
    // Anti-jam for the serial AstraHttpServer: one shared HttpClient (no per-call socket churn) with a
    // short timeout, plus a userId->expiry cache so a user who validated once doesn't re-hit graph on
    // every login. Populated only on a genuine pass, so a never-valid client is never cached in.
    private static readonly HttpClient NonceHttp = new HttpClient { Timeout = TimeSpan.FromSeconds(4) };
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, long> _nonceOkCache = new();
    private const long NONCE_CACHE_TTL_SEC = 12 * 3600;
    // Master switch. false = CAPTURE-ONLY: verify + log the token/claims/nonces but STILL issue the
    // JWT, so we confirm the real nonce mapping (server-issued AttestationNonce vs client-picked)
    // on a live login before we start rejecting anyone. Flip to true to ENFORCE once confirmed.
    // ============================================================================================
    // INSECURE TESTING MODE  (env MOTHERSHIP_INSECURE=1)
    // --------------------------------------------------------------------------------------------
    // A SIDELOADED build can never pass the normal gates, by design:
    //   * repacking + re-signing changes the signing certificate, so QUEST_EXPECTED_CERT_SHA fails
    //   * a non-store install reports app_integrity_state != "StoreRecognized", so QUEST_REQUIRE_STORE fails
    //   * changing the package id fails QUEST_EXPECTED_PACKAGE
    // and each of those additionally fires a 7-day device ban when QUEST_BAN_ON_TAMPER is on. So a
    // self-built client simply cannot log in against the production policy - which is correct for
    // production and useless for testing on your own hardware.
    //
    // Setting MOTHERSHIP_INSECURE=1 turns every one of those checks off, so any client - sideloaded,
    // re-signed, repackaged, or a headless test client with no Meta identity at all - can log in.
    // Nothing else changes: the JWT is still issued and signed, sessions still work, and the game
    // server's join gate still consults /v1/server/authorized as usual.
    //
    // NEVER run a public server with this on. It is an authentication bypass: anyone can claim any
    // identity. It is for a LAN box you control while testing your own client.
    // ============================================================================================
    static readonly bool INSECURE_TESTING =
        (Environment.GetEnvironmentVariable("MOTHERSHIP_INSECURE") ?? "") is "1" or "true" or "TRUE" or "yes";

    static readonly bool   QUEST_ENFORCE   = !INSECURE_TESTING && true;
    // RIFT nonce enforcement. CAPTURE-FIRST: verify + log (incl. the client's real body field names)
    // but still issue the session, so we confirm the Rift client actually sends a UserProof nonce and
    // which keys it uses BEFORE we start denying. Flip true to reject bad/missing nonces.
    static readonly bool   RIFT_ENFORCE    = !INSECURE_TESTING && false;
    // --- Attestation claim policy (enforced only when QUEST_ENFORCE && !trustedServer) --------------
    // A failure of any of these TAMPER checks -> deny login AND fire a device ban (below).
    static readonly string QUEST_EXPECTED_PACKAGE   = INSECURE_TESTING ? "" : "org.zern.rigel";   // package_id pin ("" = skip)
    // Cert pin left OPEN for the first real-validation pass: App Lab may re-sign the uploaded APK,
    // so the attestation's package_cert_sha256_digest may differ from our upload cert
    // (bee5fc0706704f1ed40226b7e0d95481a34f679562fcde253d2ff17bd51c1c9c). Read the actual digest
    // from the [QUESTAUTH] attestation log on a successful login, then pin it here.
    static readonly string QUEST_EXPECTED_CERT_SHA  = INSECURE_TESTING ? "" : "bee5fc0706704f1ed40226b7e0d95481a34f679562fcde253d2ff17bd51c1c9c"; // our signing cert (confirmed: Meta did not re-sign)
    // Store-recognized required for a REAL (App Lab) install. Dev-loop override MOTHERSHIP_ALLOW_NONSTORE=1
    // relaxes ONLY this one check (package/cert/proof/device still enforced) so a sideloaded hook build
    // can be tested without a 5-min App Lab re-upload each iteration. Leave OFF for the shipped build.
    static readonly bool   ALLOW_NONSTORE  =
        (Environment.GetEnvironmentVariable("MOTHERSHIP_ALLOW_NONSTORE") ?? "") is "1" or "true" or "TRUE" or "yes";
    static readonly bool   QUEST_REQUIRE_STORE      = !INSECURE_TESTING && !ALLOW_NONSTORE && true;    // require app_integrity_state == StoreRecognized
    static readonly bool   QUEST_REQUIRE_DEVICE_BASIC = !INSECURE_TESTING && true;  // require device_integrity_state in {Advanced, Basic} (reject NotTrusted)
    static readonly bool   QUEST_REQUIRE_PATCHED    = !INSECURE_TESTING && true;    // deny if security_update_pending_days >= 1
    static readonly bool   QUEST_BAN_ON_TAMPER      = !INSECURE_TESTING && true;    // fire a device ban when a tamper check fails
    const int    QUEST_BAN_MINUTES        = 7 * 24 * 60;   // 7 days

    // Shared secret between the backend and OUR game-server DLL. Baked in (private binaries).
    // Two jobs: (1) the DLL sends it so its on-behalf-of-a-player logins (quest fetch, which use a
    // DUMMY attestation) are EXEMPT from Meta verification — a real client can't present it, so a
    // real client must still attest; (2) it gates the server-only /v1/server/authorized endpoint.
    // NOTE: distinct from the legacy presence-only "x-server-api-key: x" other routes still accept.
    const string SERVER_API_KEY = "7b93a32d659d7bb73eb74c6f3f46a6a1934d47b383295a878f91ea2852d917d2";
    // Admin/automation credential for the server-only routes (moderation, automation, associations,
    // title-data, offers, deployment-automation). Distinct from SERVER_API_KEY so tooling and the
    // game server can be scoped separately if desired. Set to a strong secret; admin tools must send
    // it as x-automation-key. The GAME never calls these routes, so this doesn't affect gameplay.
    const string AUTOMATION_KEY = "a7f4e1c9b2d84f6039e5c1a7b8d2f0e6c3549a1de7b06f24839c5ad1e2f7b904";

    /// <summary>Shouts if the auth bypass is on, so an insecure box can never be mistaken for a
    /// production one. Runs once, on first touch of the class.</summary>
    static MothershipServer()
    {
        if (!INSECURE_TESTING) return;
        var prev = Console.ForegroundColor;
        Console.ForegroundColor = ConsoleColor.Red;
        Console.WriteLine();
        Console.WriteLine("  ##############################################################################");
        Console.WriteLine("  ##  THIS MOTHERSHIP IS IN INSECURE TESTING MODE (MOTHERSHIP_INSECURE=1)     ##");
        Console.WriteLine("  ##                                                                          ##");
        Console.WriteLine("  ##  Meta attestation, StoreRecognized, device-integrity, package-id and     ##");
        Console.WriteLine("  ##  signing-cert checks are ALL DISABLED. Any client can claim any          ##");
        Console.WriteLine("  ##  identity. Use only on a LAN box you control. Never expose this.         ##");
        Console.WriteLine("  ##############################################################################");
        Console.WriteLine();
        Console.ForegroundColor = prev;
    }

    public static IDatabase Database { get; set; } = null!;

    static readonly ConcurrentDictionary<string, string> _pendingQuestAuth = new();
    // Single-use guard: every attestation nonce may authenticate exactly once (anti-replay).
    static readonly ConcurrentDictionary<string, byte> _usedQuestNonces = new();
    // unique_ids we've already fired a device ban for this process — avoid re-hammering Meta on retries.
    static readonly ConcurrentDictionary<string, byte> _bannedUniqueIds = new();

    // Active-session allowlist for the game-server join gate (Tier 1). id -> expiry (unix seconds).
    // Populated only by a REAL client login (Quest attestation / RIFT), never by a server-key call.
    // The game server (DLL) checks a connecting player's id against this via /v1/server/authorized;
    // anyone who never authenticated through us isn't here -> gets kicked.
    static readonly ConcurrentDictionary<string, long> _activeSessions = new();

    /// <summary>Marks one or more ids as an active authed session (default 12h TTL).</summary>
    private static void MarkActiveSession(int hours = 12, params string?[] ids)
    {
        var exp = DateTimeOffset.UtcNow.AddHours(hours).ToUnixTimeSeconds();
        foreach (var id in ids)
            if (!string.IsNullOrEmpty(id)) _activeSessions[id!] = exp;
    }

    /// <summary>True if `id` has a non-expired active session (lazily evicts expired entries).</summary>
    private static bool IsActiveSession(string id)
    {
        if (string.IsNullOrEmpty(id)) return false;
        if (!_activeSessions.TryGetValue(id, out var exp)) return false;
        if (exp >= DateTimeOffset.UtcNow.ToUnixTimeSeconds()) return true;
        _activeSessions.TryRemove(id, out _);
        return false;
    }

    /// <summary>True if a player with this id has ever passed attestation (persisted V2 player record,
    /// keyed by ExternalProviderId or PlayerId). Durable across restarts. FindAll().Any — never
    /// FindOne(captured) — per the LiteDB translator-hang gotcha.</summary>
    private static bool PlayerPersisted(string id)
    {
        if (string.IsNullOrEmpty(id)) return false;
        var col = Database.GetCollection<MothershipV2PlayerDbObject>(true);
        return col != null && col.FindAll().Any(p => p.ExternalProviderId == id || p.PlayerId == id);
    }

    /// <summary>True if the request carries our real baked server key (constant-time compare).</summary>
    private static bool IsTrustedServer(IHttpRequest req)
    {
        var k = req.GetHeaderValue("x-server-api-key");
        if (string.IsNullOrEmpty(k) || SERVER_API_KEY.StartsWith("<FILL_ME")) return false;
        return CryptographicOperations.FixedTimeEquals(
            Encoding.UTF8.GetBytes(k), Encoding.UTF8.GetBytes(SERVER_API_KEY));
    }

    public MothershipServer() : base(HOSTNAME, PORT) { }


    private static string NewId() => Guid.NewGuid().ToString();

    private static long ExpirationSec(int hours = 24)  => DateTimeOffset.UtcNow.AddHours(hours).ToUnixTimeSeconds();
    private static long ExpirationMs(int hours  = 1)   => DateTimeOffset.UtcNow.AddHours(hours).ToUnixTimeMilliseconds();

    private static bool CtEquals(string? a, string b)
        => !string.IsNullOrEmpty(a) &&
           CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(a), Encoding.UTF8.GetBytes(b));

    /// <summary>Admin/server scope: the request must carry the REAL server or automation key (value,
    /// constant-time) — NOT merely present, and NOT a client JWT. Use on all server/automation/
    /// moderation-server/title-data/offers/association routes.</summary>
    private static bool IsAdmin(IHttpRequest req)
        => CtEquals(req.GetHeaderValue("x-server-api-key"), SERVER_API_KEY)
        || CtEquals(req.GetHeaderValue("x-automation-key"), AUTOMATION_KEY);

    /// <summary>Client scope: a valid, signature-checked player JWT (x-mothership-token). Use on
    /// player-facing routes (userdata, client report, shared-group).</summary>
    private static bool IsClient(IHttpRequest req)
        => !string.IsNullOrEmpty(GetUserIdFromToken(req));

    /// <summary>Either a trusted admin/server key OR a valid client JWT.</summary>
    private static bool IsAuthorized(IHttpRequest req) => IsAdmin(req) || IsClient(req);

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

    /// <summary>Validates the HS256 signature and returns the 'sub' claim, or "" if invalid.</summary>
    private static string GetUserIdFromToken(IHttpRequest req)
    {
        var token = req.GetHeaderValue("x-mothership-token");
        if (string.IsNullOrEmpty(token)) return "";

        var parts = token.Split('.');
        if (parts.Length != 3) return "";

        // verify signature
        using var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(JWT_SECRET));
        var expected = Base64Url(hmac.ComputeHash(Encoding.UTF8.GetBytes($"{parts[0]}.{parts[1]}")));
        if (!CryptographicOperations.FixedTimeEquals(
                Encoding.UTF8.GetBytes(expected), Encoding.UTF8.GetBytes(parts[2])))
            return "";

        // decode payload
        string p = parts[1].Replace('-', '+').Replace('_', '/');
        switch (p.Length % 4) { case 2: p += "=="; break; case 3: p += "="; break; }
        try
        {
            using var doc = JsonDocument.Parse(Convert.FromBase64String(p));
            var root = doc.RootElement;

            // optional: honor expiry
            if (root.TryGetProperty("exp", out var exp) &&
                exp.GetInt64() < DateTimeOffset.UtcNow.ToUnixTimeSeconds())
                return "";

            return root.TryGetProperty("sub", out var sub) ? sub.GetString() ?? "" : "";
        }
        catch { return ""; }
    }

    private static string Base64Url(byte[] input) =>
        Convert.ToBase64String(input).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    /// <summary>Decodes a Base64URL (no-padding) string to UTF-8 text.</summary>
    private static string DecodeBase64UrlToString(string input)
    {
        string s = input.Replace('-', '+').Replace('_', '/');
        switch (s.Length % 4) { case 2: s += "=="; break; case 3: s += "="; break; }
        return Encoding.UTF8.GetString(Convert.FromBase64String(s));
    }

    /// <summary>Result of a Meta Platform Integrity token verification.</summary>
    private sealed record AttestationResult(
        bool Ok, string Message, string Nonce, long Exp,
        string PackageId, string AppIntegrity, string DeviceIntegrity, string RawClaims,
        long Timestamp = 0, string UniqueId = "", int SecurityPendingDays = 0,
        bool DeviceBanned = false, string CertDigestsCsv = "");

    /// <summary>Verifies a Quest attestation token against Meta's attestation server and decodes
    /// its claims. Ok=false on any network/parse failure or a non-"success" message — the caller
    /// decides whether to enforce. Never throws.</summary>
    private static async Task<AttestationResult> VerifyQuestAttestation(string attestationToken)
    {
        try
        {
            var accessToken = $"OC|{META_APP_ID}|{META_APP_SECRET}";
            var url = "https://graph.oculus.com/platform_integrity/verify"
                    + $"?token={Uri.EscapeDataString(attestationToken)}"
                    + $"&access_token={Uri.EscapeDataString(accessToken)}";

            // NOTE: AstraHttpServer processes serially — keep this short so a slow Meta response
            // can't jam the whole backend for long.
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
            var resp = await http.GetAsync(url);
            var text = await resp.Content.ReadAsStringAsync();

            using var doc = JsonDocument.Parse(text);
            if (!doc.RootElement.TryGetProperty("data", out var data) ||
                data.ValueKind != JsonValueKind.Array || data.GetArrayLength() == 0)
                return new AttestationResult(false, "malformed response", "", 0, "", "", "", text);

            var first   = data[0];
            var message = first.TryGetProperty("message", out var m) ? m.GetString() ?? "" : "";
            if (message != "success" || !first.TryGetProperty("claims", out var cl))
                return new AttestationResult(false, string.IsNullOrEmpty(message) ? "no message" : message,
                                             "", 0, "", "", "", text);

            var claimsJson = DecodeBase64UrlToString(cl.GetString() ?? "");
            using var cdoc = JsonDocument.Parse(claimsJson);
            var root = cdoc.RootElement;

            string nonce = ""; long exp = 0, ts = 0;
            if (root.TryGetProperty("request_details", out var rd))
            {
                nonce = rd.TryGetProperty("nonce",     out var n) ? n.GetString() ?? "" : "";
                exp   = rd.TryGetProperty("exp",       out var e) ? e.GetInt64()    : 0;
                ts    = rd.TryGetProperty("timestamp", out var t) ? t.GetInt64()    : 0;
            }
            string pkg = "", appI = "", certCsv = "";
            if (root.TryGetProperty("app_state", out var app))
            {
                pkg  = app.TryGetProperty("package_id",          out var p) ? p.GetString() ?? "" : "";
                appI = app.TryGetProperty("app_integrity_state", out var a) ? a.GetString() ?? "" : "";
                if (app.TryGetProperty("package_cert_sha256_digest", out var cd) && cd.ValueKind == JsonValueKind.Array)
                    certCsv = string.Join(",", cd.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()));
            }
            string devI = "", uid = ""; int pending = 0;
            if (root.TryGetProperty("device_state", out var dev))
            {
                devI    = dev.TryGetProperty("device_integrity_state",     out var d) ? d.GetString() ?? "" : "";
                uid     = dev.TryGetProperty("unique_id",                  out var u) ? u.GetString() ?? "" : "";
                pending = dev.TryGetProperty("security_update_pending_days", out var s) ? s.GetInt32() : 0;
            }
            // device_ban section is present at root ONLY when the device is banned.
            bool banned = false;
            if (root.TryGetProperty("device_ban", out var db2) && db2.ValueKind == JsonValueKind.Object)
                banned = db2.TryGetProperty("is_banned", out var ib) &&
                         (ib.ValueKind == JsonValueKind.True || (ib.ValueKind == JsonValueKind.String && ib.GetString() == "true"));

            return new AttestationResult(true, "success", nonce, exp, pkg, appI, devI, claimsJson,
                                         ts, uid, pending, banned, certCsv);
        }
        catch (Exception ex)
        {
            return new AttestationResult(false, $"exception: {ex.Message}", "", 0, "", "", "", "");
        }
    }

    /// <summary>Validates a Meta UserProof nonce (ovr_User_GetUserProof) for a given oculus user_id
    /// against Meta's graph endpoint — proves the caller genuinely owns that Meta account. Returns
    /// (ok, rawResponse). Never throws.</summary>
    private static async Task<(bool Ok, string Raw)> VerifyMetaUserNonce(string userId, string nonce)
    {
        // A UserProof nonce is only valid under the app that minted it, and we can't tell Quest from
        // Rift at this layer — so try the Quest creds first, then the Rift creds. Valid on EITHER =>
        // ok; only if BOTH reject do we treat the nonce as invalid (caller denies the login).
        if (string.IsNullOrEmpty(userId) || string.IsNullOrEmpty(nonce))
            return (false, "missing userId/nonce");
        var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        if (_nonceOkCache.TryGetValue(userId, out var exp) && exp > now) return (true, "cache");   // no I/O
        if (exp != 0 && exp <= now) _nonceOkCache.TryRemove(userId, out _);
        var q = await VerifyMetaUserNonceWith($"OC|{META_APP_ID}|{META_APP_SECRET}", userId, nonce);
        if (q.Ok) { _nonceOkCache[userId] = now + NONCE_CACHE_TTL_SEC; return (true, "app=quest " + q.Raw); }
        var r = await VerifyMetaUserNonceWith($"OC|{RIFT_APP_ID}|{RIFT_APP_SECRET}", userId, nonce);
        if (r.Ok) { _nonceOkCache[userId] = now + NONCE_CACHE_TTL_SEC; return (true, "app=rift " + r.Raw); }
        return (false, $"neither app valid (quest={q.Raw} | rift={r.Raw})");
    }

    /// <summary>Single-app UserProof nonce check against graph.oculus.com/user_nonce_validate. Never throws.</summary>
    private static async Task<(bool Ok, string Raw)> VerifyMetaUserNonceWith(string accessToken, string userId, string nonce)
    {
        try
        {
            var form = new FormUrlEncodedContent(new Dictionary<string, string>
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
            return (ok, text);
        }
        catch (Exception ex)
        {
            return (false, $"exception: {ex.Message}");
        }
    }

    /// <summary>Fire a Meta device ban on a token's unique_id (server-to-server). Fire-and-forget from
    /// the caller so it never blocks the login response (the serial-server jam lesson). Idempotent per
    /// process via _bannedUniqueIds. NOTE: requires the app's "Device Ban" Data-Use-Checkup approval on
    /// the Meta dashboard — until approved this call returns an error, which we just log. Never throws.</summary>
    private static async Task BanDevice(string uniqueId, int minutes, string reason)
    {
        if (string.IsNullOrEmpty(uniqueId)) return;
        if (!_bannedUniqueIds.TryAdd(uniqueId, 1)) return;   // already banned this session
        try
        {
            var accessToken = $"OC|{META_APP_ID}|{META_APP_SECRET}";
            var url = "https://graph.oculus.com/platform_integrity/device_ban?method=POST"
                    + $"&unique_id={Uri.EscapeDataString(uniqueId)}"
                    + $"&is_banned=true&remaining_time_in_minute={minutes}"
                    + $"&access_token={Uri.EscapeDataString(accessToken)}";
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
            var resp = await http.PostAsync(url, null);
            var text = await resp.Content.ReadAsStringAsync();
            Console.WriteLine($"[QUEST-BAN] unique_id={uniqueId} minutes={minutes} reason='{reason}' -> {text}");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[QUEST-BAN] unique_id={uniqueId} error: {ex.Message}");
        }
    }

    private static IHttpActionResult Json<T>(T obj, HttpStatusCode code = HttpStatusCode.OK) =>
        Results.Configurable(code, "application/json", JsonSerializer.SerializeToUtf8Bytes<T>(obj));

    private static IHttpActionResult JsonAnon(object obj, HttpStatusCode code = HttpStatusCode.OK) =>
        Results.Configurable(code, "application/json", JsonSerializer.SerializeToUtf8Bytes(obj));

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

    /// <summary>Look up an existing v1 player by external account id (or create one),
    /// refresh its session token, and persist. Keeps the mothership player id stable
    /// across logins so nothing is regenerated each time.</summary>
    private static MothershipPlayerDbObject UpsertV1Player(string externalId, string? service, string nickname)
    {
        var col      = Database.GetCollection<MothershipPlayerDbObject>(true)!;
        var existing = !string.IsNullOrEmpty(externalId)
            ? col.FindAll().FirstOrDefault(p => p.ExternalAccountId == externalId)
            : null;

        var playerId = existing?.MothershipPlayerId ?? NewId();
        var token    = GenerateSessionJwt(playerId, service, string.IsNullOrEmpty(externalId) ? null : externalId);

        var player = existing ?? new MothershipPlayerDbObject();
        player.MothershipPlayerId = playerId;
        player.ExternalAccountId  = externalId;
        if (!string.IsNullOrEmpty(nickname) || existing == null)
            player.ExternalAccountNickname = nickname;
        player.ExpirationTime = DateTimeOffset.UtcNow.AddHours(24).ToUnixTimeSeconds();
        player.Tags           = existing?.Tags ?? new();
        player.Token          = token;

        if (existing == null) col.Insert(player);
        else                  col.Update(player);

        Database.GetCollection<MothershipSessionDbObject>(true)!
            .Insert(new MothershipSessionDbObject { Token = token, PlayerId = playerId });

        return player;
    }


    // DISABLED (2026-08-20): the Insecure_1/2 routes minted a full session for any accountId sent
    // in the body with zero verification — a complete auth bypass (impersonate anyone). Return 403
    // instead of removing the route, so a caller gets a clear rejection rather than a 404 that looks
    // like a deploy problem.
    [HttpPost("/v1/client/player/auth/Insecure_1")]
    public Task<IHttpActionResult> ClientAuthInsecure1(IHttpRequest request, IHttpResponse response)
        => Task.FromResult(JsonAnon(new { error = "insecure auth disabled" }, HttpStatusCode.Forbidden));

    [HttpPost("/v1/client/player/auth/Insecure_2")]
    public Task<IHttpActionResult> ClientAuthInsecure2(IHttpRequest request, IHttpResponse response)
        => Task.FromResult(JsonAnon(new { error = "insecure auth disabled" }, HttpStatusCode.Forbidden));

    // DISABLED (2026-08-20, prod): these v1 social routes minted a full session for ANY id in the
    // body with zero verification — identity spoofing (mint a JWT for any user -> read their userdata
    // + pass admin endpoints). The real Quest path is v2 begin/complete with Meta attestation. 403.
    [HttpPost("/v1/client/player/auth/GOOGLE")]
    public Task<IHttpActionResult> ClientAuthGoogle(IHttpRequest request, IHttpResponse response)
        => Task.FromResult(JsonAnon(new { error = "auth method disabled" }, HttpStatusCode.Forbidden));

    [HttpPost("/v1/client/player/auth/APPLE")]
    public Task<IHttpActionResult> ClientAuthApple(IHttpRequest request, IHttpResponse response)
        => Task.FromResult(JsonAnon(new { error = "auth method disabled" }, HttpStatusCode.Forbidden));

    [HttpPost("/v1/client/player/auth/QUEST")]
    public Task<IHttpActionResult> ClientAuthQuest(IHttpRequest request, IHttpResponse response)
        => Task.FromResult(JsonAnon(new { error = "auth method disabled" }, HttpStatusCode.Forbidden));

    /// <summary>Authenticates a player via Oculus Rift with a fixed external ID.</summary>
    [HttpPost("/v1/client/player/auth/RIFT")]
    public async Task<IHttpActionResult> ClientAuthRift(IHttpRequest request, IHttpResponse response)
    {
        const string RIFT_EXTERNAL_ID   = "4649330831842445";
        const string RIFT_ORG_SCOPED_ID = "4322323827863641";

        // Prove the caller owns their Meta account before issuing a RIFT session. The client sends its
        // real oculus user id + a UserProof nonce; validate against BOTH app cred sets (Rift matches
        // under the Rift app, Quest under the Quest app) and deny if neither accepts. Trusted-server
        // calls (real SERVER_API_KEY) are exempt. bodyKeys is logged so we can confirm the client's
        // actual field names on a live login and adjust the lookups if needed.
        if (RIFT_ENFORCE && !IsTrustedServer(request))   // no graph call on RIFT logins until we enable it
        {
            var body  = ParseBody(request);
            var uid   = Str(body, "UserId");
            if (string.IsNullOrEmpty(uid))   uid   = Str(body, "user_id");
            if (string.IsNullOrEmpty(uid))   uid   = Str(body, "AccountId");
            if (string.IsNullOrEmpty(uid))   uid   = Str(body, "accountId");
            if (string.IsNullOrEmpty(uid))   uid   = Str(body, "platform_id");
            var nonce = Str(body, "Nonce");
            if (string.IsNullOrEmpty(nonce)) nonce = Str(body, "nonce");
            if (string.IsNullOrEmpty(nonce)) nonce = Str(body, "MetaNonce");
            if (string.IsNullOrEmpty(nonce)) nonce = Str(body, "user_proof_nonce");

            var proof = await VerifyMetaUserNonce(uid, nonce);
            Console.WriteLine($"[RIFT-AUTH] uid='{uid}' nonceLen={nonce.Length} proofOk={proof.Ok} enforce={RIFT_ENFORCE} raw={proof.Raw} bodyKeys=[{string.Join(",", body.Keys)}]");
            if (RIFT_ENFORCE && !proof.Ok)
                return JsonAnon(new { error = "rift nonce verification failed", code = 401 }, HttpStatusCode.Unauthorized);
        }

        var col      = Database.GetCollection<MothershipV2PlayerDbObject>(true)!;
        var existing = col.FindAll().FirstOrDefault(p => p.ExternalProviderId == RIFT_EXTERNAL_ID && p.ExternalService == "RIFT");
        var playerId = existing?.PlayerId ?? NewId();

        var now    = DateTimeOffset.UtcNow;
        var expSec = now.AddHours(1).ToUnixTimeSeconds();
        var expMs  = now.AddHours(1).ToUnixTimeMilliseconds();

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

        var player = existing ?? new MothershipV2PlayerDbObject();
        player.ExternalProviderId       = RIFT_EXTERNAL_ID;
        player.ExternalProviderUsername = existing?.ExternalProviderUsername ?? "";
        player.IsPrimaryId              = true;
        player.PlayerId                 = playerId;
        player.Tags                     = null;
        player.Token                    = token;
        player.ExpirationTime           = expMs;
        player.ExternalService          = "RIFT";
        if (existing == null) col.Insert(player);
        else                  col.Update(player);

        // Tier 1 join gate: RIFT is a shared fixed identity with no attestation, but a RIFT login
        // still came through us — mark its ids active so RIFT players aren't kicked by the gate.
        MarkActiveSession(12, RIFT_EXTERNAL_ID, RIFT_ORG_SCOPED_ID, playerId);

        return Json(player, HttpStatusCode.Created);
    }


    /// <summary>Game-server join gate (Tier 1): is this player id a currently-active authed session?
    /// Server-only — requires the real SERVER_API_KEY. The DLL calls this on a connecting player's
    /// id (read off their controller) and kicks anyone who comes back not authorized.</summary>
    [HttpGet("/v1/server/authorized")]
    public Task<IHttpActionResult> ServerAuthorized(IHttpRequest request, IHttpResponse response)
    {
        if (!IsTrustedServer(request))
            return Task.FromResult(JsonAnon(new { error = "forbidden" }, HttpStatusCode.Forbidden));
        var id = request.GetQueryParameter("id") ?? "";
        // Authorized if a LIVE session (in-memory) OR a PERSISTED player record exists. The persisted
        // record only gets written AFTER attestation passes (CompleteQuestAuth upsert is past the
        // enforce block), so it's a durable "this id authenticated through us" signal that survives a
        // backend restart — without it, restarting the backend would kick every already-connected
        // player (in-memory _activeSessions is wiped on restart). Never-authed clients have neither.
        bool ok = IsActiveSession(id) || PlayerPersisted(id);
        return Task.FromResult(JsonAnon(new { authorized = ok }));
    }


    /// <summary>Accepts a client analytics event batch (no-op, returns empty result).</summary>
    [HttpPost("/v1/client/analytics/event/batch")]
    public async Task<IHttpActionResult> AnalyticsBatch(IHttpRequest request, IHttpResponse response)
    {
        return JsonAnon(new { event_id = "not found", results = Array.Empty<object>() });
    }


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

        // --- Meta Platform Integrity verification --------------------------------------------------
        // The client obtained `attestationToken` via DeviceApplicationIntegrity::GetIntegrityToken.
        // Verify its signature with Meta and bind the nonce inside the (Meta-signed) claims to a
        // nonce we can trust, single-use, so a stolen token can't be replayed.
        var issuedNonce = _pendingQuestAuth.TryGetValue(userId, out var pn) ? pn : "";

        // Trusted-server calls (our DLL fetching a player's userdata) carry the real SERVER_API_KEY
        // and use a dummy attestation — exempt them from Meta verification, and never let them mark
        // an active session (only a genuine client login does that).
        var trustedServer = IsTrustedServer(request);

        // CRITICAL: only touch graph.oculus.com when we're ACTUALLY going to enforce a real client.
        // AstraHttpServer serializes badly under blocking I/O — doing these two outbound calls on
        // every complete/QUEST (incl. the DLL's frequent trusted-server quest-fetch logins, and
        // capture mode) parked threads on graph and JAMMED the whole Mothership server, so begin/QUEST
        // and userdata for real clients timed out. Skip them entirely unless enforcing a real client.
        AttestationResult att = new(false, "skipped", "", 0, "", "", "", "");
        (bool Ok, string Raw) proof = (false, "skipped");
        if (QUEST_ENFORCE && !trustedServer)
        {
            // (a) Device/app integrity + challenge-nonce binding.
            att = await VerifyQuestAttestation(attestationToken);
            // (b) Identity: the MetaNonce is a UserProof nonce — Meta confirms this userId owns the account.
            proof = await VerifyMetaUserNonce(userId, metaNonce);
        }

        // CAPTURE (non-blocking, no graph call): dump the RAW token + nonces from a real client so we
        // can replay it against graph.oculus.com offline and see why enforcement would pass/fail
        // (app-creds? nonce binding? package pin? token format?). Skipped for the trusted-server's
        // dummy "x" token. Remove once enforcement is validated.
        Console.WriteLine(
            $"[QUEST-ATT] userId={userId} attOk={att.Ok} attMsg={att.Message} " +
            $"claimNonce={att.Nonce} metaNonce={metaNonce} issuedNonce={issuedNonce} " +
            $"exp={att.Exp} ts={att.Timestamp} pkg={att.PackageId} app={att.AppIntegrity} dev={att.DeviceIntegrity} " +
            $"patchDays={att.SecurityPendingDays} banned={att.DeviceBanned} uid={att.UniqueId} cert={att.CertDigestsCsv} " +
            $"proofOk={proof.Ok} proofRaw={proof.Raw} enforce={QUEST_ENFORCE} trustedServer={trustedServer}");

        if (QUEST_ENFORCE && !trustedServer)
        {
            // --- (b) UserProof: prove the userId genuinely owns the Meta account -------------------
            if (!proof.Ok)
                return JsonAnon(new { error = "userproof nonce invalid" }, HttpStatusCode.Unauthorized);
            // MetaNonce is single-use too (Meta also invalidates it, but guard our side).
            if (string.IsNullOrEmpty(metaNonce) || !_usedQuestNonces.TryAdd("mp:" + metaNonce, 1))
                return JsonAnon(new { error = "userproof nonce replayed" }, HttpStatusCode.Unauthorized);

            // --- (a) Attestation: device/app integrity + challenge-nonce binding -------------------
            if (!att.Ok)
                return JsonAnon(new { error = $"attestation verify failed: {att.Message}" }, HttpStatusCode.Unauthorized);

            // The nonce Meta signed into the claims must match the challenge we issued in begin/QUEST
            // (server-derived, strongest); fall back to the client-reported nonce only if we issued
            // none — the first real capture tells us which mode the client actually uses.
            bool bound = (!string.IsNullOrEmpty(issuedNonce) && att.Nonce == issuedNonce)
                      || (string.IsNullOrEmpty(issuedNonce) && !string.IsNullOrEmpty(att.Nonce));
            if (!bound)
                return JsonAnon(new { error = "attestation nonce mismatch" }, HttpStatusCode.Unauthorized);

            // Single-use on the challenge nonce.
            if (string.IsNullOrEmpty(att.Nonce) || !_usedQuestNonces.TryAdd("att:" + att.Nonce, 1))
                return JsonAnon(new { error = "attestation nonce replayed" }, HttpStatusCode.Unauthorized);

            // Freshness (Meta enforces 24h, but be explicit).
            if (att.Exp != 0 && att.Exp < DateTimeOffset.UtcNow.ToUnixTimeSeconds())
                return JsonAnon(new { error = "attestation expired" }, HttpStatusCode.Unauthorized);

            // Meta already banned this device -> deny (no re-ban; the ban is live on Meta's side).
            if (att.DeviceBanned)
            {
                Console.WriteLine($"[QUEST-ATT] DENY userId={userId} reason='device_ban.is_banned' uid={att.UniqueId}");
                return JsonAnon(new { error = "device banned" }, HttpStatusCode.Unauthorized);
            }

            // --- TAMPER checks: any failure denies AND fires a 7-day device ban on the token's uid ---
            string tamper = null;
            if (!string.IsNullOrEmpty(QUEST_EXPECTED_PACKAGE) && att.PackageId != QUEST_EXPECTED_PACKAGE)
                tamper = $"package_id={att.PackageId}";
            else if (QUEST_REQUIRE_STORE && att.AppIntegrity != "StoreRecognized")
                tamper = $"app_integrity={att.AppIntegrity}";
            else if (!string.IsNullOrEmpty(QUEST_EXPECTED_CERT_SHA) &&
                     !(att.CertDigestsCsv ?? "").Split(',').Contains(QUEST_EXPECTED_CERT_SHA))
                tamper = "cert_digest_mismatch";
            else if (QUEST_REQUIRE_DEVICE_BASIC && att.DeviceIntegrity != "Advanced" && att.DeviceIntegrity != "Basic")
                tamper = $"device_integrity={att.DeviceIntegrity}";
            else if (QUEST_REQUIRE_PATCHED && att.SecurityPendingDays >= 1)
                tamper = $"security_update_pending_days={att.SecurityPendingDays}";

            if (tamper != null)
            {
                if (QUEST_BAN_ON_TAMPER && !string.IsNullOrEmpty(att.UniqueId))
                    _ = BanDevice(att.UniqueId, QUEST_BAN_MINUTES, tamper);   // fire-and-forget (don't jam the response)
                Console.WriteLine($"[QUEST-ATT] DENY+BAN userId={userId} reason='{tamper}' uid={att.UniqueId}");
                return JsonAnon(new { error = $"attestation policy failed: {tamper}" }, HttpStatusCode.Unauthorized);
            }
        }

        var col      = Database.GetCollection<MothershipV2PlayerDbObject>(true)!;
        var existing = col.FindAll().FirstOrDefault(p => p.ExternalProviderId == userId && p.ExternalService == "QUEST");
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

        // Tier 1 join gate: a genuine client login (not a trusted-server userdata fetch) is now an
        // active session. Key on every id the game server might read off the connecting player.
        if (!trustedServer)
            MarkActiveSession(12, userId, playerId);

        _pendingQuestAuth.TryRemove(userId, out _);
        return Json(player, HttpStatusCode.Created);
    }


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

        var steamId  = "76561199232028535";
        var col      = Database.GetCollection<MothershipV2PlayerDbObject>(true)!;
        var existing = col.FindAll().FirstOrDefault(p => p.ExternalProviderId == steamId && p.ExternalService == "STEAM");
        var playerId = existing?.PlayerId ?? NewId();
        var deployId = request.GetHeaderValue("x-mothership-deployment-id") ?? NewId();
        var envId    = request.GetHeaderValue("x-mothership-env-id")        ?? ENVIRONMENT_ID;
        var titleId  = request.GetHeaderValue("x-mothership-title-id")      ?? TENANT_ID;

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

        var player = existing ?? new MothershipV2PlayerDbObject();
        player.ExternalProviderId       = steamId;
        player.ExternalProviderUsername = existing?.ExternalProviderUsername ?? "drycheetah84";
        player.IsPrimaryId              = true;
        player.PlayerId                 = playerId;
        player.Tags                     = null;
        player.Token                    = GenerateJwt(jwtPayload);
        player.ExpirationTime           = expMs;
        player.ExternalService          = "STEAM";
        if (existing == null) col.Insert(player);
        else                  col.Update(player);

        return Json(player, HttpStatusCode.Created);
    }


    /// <summary>Verifies a player session token and returns the associated player.</summary>
    [HttpPost("/v1/server/player/auth/verify_token")]
    public async Task<IHttpActionResult> VerifyToken(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipAssociationDbObject>(true)!
            .Find(a => a.PlayerId == player_id).ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes all associations for a given player ID.</summary>
    [HttpDelete("/v1/server/player/auth/associations/{player_id}")]
    public async Task<IHttpActionResult> DeleteAssociations(IHttpRequest request, IHttpResponse response, string player_id)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        Database.GetCollection<MothershipAssociationDbObject>(true)!.DeleteMany(a => a.PlayerId == player_id);
        return JsonAnon(new { status = "deleted" });
    }


    /// <summary>Lists all players (automation).</summary>
    [HttpGet("/v1/automation/player/auth/players")]
    public async Task<IHttpActionResult> AutomationGetPlayers(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipPlayerDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Validates a username and returns a placeholder user (automation).</summary>
    [HttpPost("/v1/automation/player/auth/players")]
    public async Task<IHttpActionResult> AutomationValidatePlayer(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var body     = ParseBody(request);
        var username = Str(body, "username");
        return JsonAnon(new { username, user_id = NewId(), recent_flagged_usernames = Array.Empty<string>() });
    }

    /// <summary>Updates tags on a player record (automation).</summary>
    [HttpPost("/v1/automation/player/auth/players/tags")]
    public async Task<IHttpActionResult> AutomationUpdatePlayerTags(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var playerId = request.GetQueryParameter("player_id") ?? "";
        var links    = Database.GetCollection<MothershipLinkDbObject>(true)!
            .Find(l => l.TargetPlayer == playerId).ToList();
        return JsonAnon(new { Identities = links });
    }

    /// <summary>Deletes an account link by ID (automation).</summary>
    [HttpDelete("/v1/automation/player/auth/links/{platform}/{link_id}")]
    public async Task<IHttpActionResult> AutomationDeleteLink(IHttpRequest request, IHttpResponse response, string platform, string link_id)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var mothershipId = request.GetQueryParameter("mothershipId") ?? "";
        var results      = Database.GetCollection<MothershipAssociationDbObject>(true)!
            .Find(a => a.PlayerId == mothershipId).ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a specific association by ID (automation).</summary>
    [HttpDelete("/v1/automation/player/auth/associations/{platform}/{assoc_id}")]
    public async Task<IHttpActionResult> AutomationDeleteAssociation(IHttpRequest request, IHttpResponse response, string platform, string assoc_id)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var col   = Database.GetCollection<MothershipAssociationDbObject>(true)!;
        var assoc = col.FindOne(a => a.AssociationId == assoc_id);
        if (assoc == null) return JsonAnon(new { error = "Association not found" }, HttpStatusCode.NotFound);

        col.Delete(assoc.Id);
        return JsonAnon(new { status = "deleted" });
    }


    /// <summary>Explicitly links two accounts on a given platform.</summary>
    [HttpPost("/v1/{platform}/player/auth/explicit_link")]
    public async Task<IHttpActionResult> ExplicitLink(IHttpRequest request, IHttpResponse response, string platform)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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


    /// <summary>Submits a moderation report from a client.</summary>
    [HttpPost("/v1/moderation/client/report")]
    public async Task<IHttpActionResult> ClientCreateReport(IHttpRequest request, IHttpResponse response)
    {
        if (!IsClient(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);   // player-facing: valid JWT

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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


    /// <summary>Returns all bans, mutes and reports (automation).</summary>
    [HttpGet("/v1/moderation/automation")]
    public async Task<IHttpActionResult> GetModerationAutomation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var bans    = Database.GetCollection<MothershipBanDbObject>(true)!.FindAll().ToList();
        var mutes   = Database.GetCollection<MothershipMuteDbObject>(true)!.FindAll().ToList();
        var reports = Database.GetCollection<MothershipReportDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { bans, mutes, reports });
    }

    /// <summary>Creates a mute or other moderation action (automation).</summary>
    [HttpPost("/v1/moderation/automation")]
    public async Task<IHttpActionResult> PostModerationAutomation(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var muteId = request.GetQueryParameter("mute_id");
        if (muteId == null) return JsonAnon(new { error = "Mute not found" }, HttpStatusCode.NotFound);

        var col  = Database.GetCollection<MothershipMuteDbObject>(true)!;
        var mute = col.FindOne(m => m.MuteId == muteId);
        if (mute == null) return JsonAnon(new { error = "Mute not found" }, HttpStatusCode.NotFound);

        col.Delete(mute.Id);
        return JsonAnon(new { status = "deleted" });
    }


    /// <summary>Creates a shared group (server).</summary>
    [HttpPost("/v1/server/shared-group")]
    public async Task<IHttpActionResult> ServerSharedGroup(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
    // ── GENERIC SHARED GROUP ROUTES ───────────────────────────────────────────

    [HttpGet("/v1/shared-group/{group_id}")]
    public async Task<IHttpActionResult> GetSharedGroup(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var group = Database.GetCollection<MothershipSharedGroupDbObject>(true)!.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);
        return Json(group);
    }

    [HttpPost("/v1/shared-group/{group_id}")]
    public async Task<IHttpActionResult> UpdateSharedGroup(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var body = ParseBody(request);
        var col = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
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

    [HttpDelete("/v1/shared-group/{group_id}")]
    public async Task<IHttpActionResult> DeleteSharedGroup(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var col = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        col.Delete(group.Id);
        return JsonAnon(new { status = "deleted" });
    }

    [HttpGet("/v1/shared-group/{group_id}/data")]
    public async Task<IHttpActionResult> GetSharedGroupData(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var group = Database.GetCollection<MothershipSharedGroupDbObject>(true)!.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);
        return Json(group);
    }

    [HttpPost("/v1/shared-group/{group_id}/members")]
    public async Task<IHttpActionResult> AddSharedGroupMembers(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var body = ParseBody(request);
        var col = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
        if (group == null) return JsonAnon(new { error = "Shared group not found" }, HttpStatusCode.NotFound);

        group.Members.AddRange(StrList(body, "members"));
        col.Update(group);
        return Json(group);
    }

    [HttpDelete("/v1/shared-group/{group_id}/members")]
    public async Task<IHttpActionResult> RemoveSharedGroupMembers(IHttpRequest request, IHttpResponse response, string group_id)
    {
        if (!IsAuthorized(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);
        var body = ParseBody(request);
        var col = Database.GetCollection<MothershipSharedGroupDbObject>(true)!;
        var group = col.FindOne(g => g.SharedGroupId == group_id);
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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipLinkDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── TITLE AUTOMATION ─────────────────────────────────────────────────────

    /// <summary>Creates a new title (automation).</summary>
    [HttpPost("/v1/title/automation")]
    public async Task<IHttpActionResult> AutomationCreateTitle(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
    /// <summary>Client-scoped user data GET. This is what the game calls. user_id from the JWT.</summary>
    [HttpGet("/v1/userdata/client")]
    public async Task<IHttpActionResult> GetUserDataClient(IHttpRequest request, IHttpResponse response)
    {
        if (!IsClient(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);   // JWT-keyed

        var userId = GetUserIdFromToken(request);
        var keyName = request.GetQueryParameter("key_name") ?? "";
        if (string.IsNullOrEmpty(userId) || string.IsNullOrEmpty(keyName))
            return JsonAnon(new { error = "Missing key_name or token subject" }, HttpStatusCode.BadRequest);

        var col = Database.GetCollection<MothershipUserDataDbObject>(true)!;
        var item = col.FindOne(d => d.UserId == userId && d.KeyName == keyName);
        if (item == null) return JsonAnon(new { error = "User data not found" }, HttpStatusCode.NotFound);
        return Json(item);
    }

    /// <summary>Client-scoped user data write. user_id from the JWT, not the body.</summary>
    [HttpPost("/v1/userdata/client")]
    public async Task<IHttpActionResult> PostUserDataClient(IHttpRequest request, IHttpResponse response)
    {
        if (!IsClient(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);   // JWT-keyed

        var body = ParseBody(request);
        var userId = GetUserIdFromToken(request);
        var keyName = Str(body, "key_name");
        if (string.IsNullOrEmpty(userId) || string.IsNullOrEmpty(keyName))
            return JsonAnon(new { error = "Missing key_name or token subject" }, HttpStatusCode.BadRequest);

        var col = Database.GetCollection<MothershipUserDataDbObject>(true)!;
        var existing = col.FindOne(d => d.UserId == userId && d.KeyName == keyName);
        if (existing != null)
        {
            existing.Value = Str(body, "value");
            existing.Generation = existing.Generation + 1;
            col.Update(existing);
            return Json(existing);
        }

        var data = new MothershipUserDataDbObject
        {
            DataId = NewId(),
            UserId = userId,
            KeyName = keyName,
            Value = Str(body, "value"),
            Generation = 1
        };
        col.Insert(data);
        return Json(data);
    }

    /// <summary>Deletes a specific user data key for a player.</summary>
    [HttpDelete("/v1/userdata")]
    public async Task<IHttpActionResult> DeleteUserData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsClient(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);   // JWT-keyed

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipUserDataMetadataDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── TITLE DATA ────────────────────────────────────────────────────────────

    /// <summary>Creates or updates a title data key-value pair.</summary>
    [HttpPost("/v1/title-data")]
    public async Task<IHttpActionResult> PostTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipTitleDataDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a title data entry by key (automation).</summary>
    [HttpDelete("/v1/title-data/automation")]
    public async Task<IHttpActionResult> AutomationDeleteTitleData(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Creates a binding between an offer and a deployment (automation).</summary>
    [HttpPost("/v1/offerbindings/automation")]
    public async Task<IHttpActionResult> AutomationCreateOfferBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferBindingDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // Note: Python had a trailing-slash variant of this route used for committing a binding
    /// <summary>Sets the committed flag on an offer binding.</summary>
    [HttpPost("/v1/offerbindings/automation/commit")]
    public async Task<IHttpActionResult> AutomationCommitOfferBinding(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── PURCHASE ──────────────────────────────────────────────────────────────

    /// <summary>Processes a client purchase by offer ID.</summary>
    [HttpPost("/v1/purchase/client")]
    public async Task<IHttpActionResult> ClientPurchase(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { status = "refreshed", timestamp = DateTime.UtcNow.ToString("O") });
    }

    /// <summary>Processes a server-side purchase and returns a transaction ID.</summary>
    [HttpPost("/v1/purchase")]
    public async Task<IHttpActionResult> ServerPurchase(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { Results = Array.Empty<object>() });
    }

    // ── INVENTORY ─────────────────────────────────────────────────────────────

    /// <summary>Returns inventory items for a player.</summary>
    [HttpGet("/v1/inventory")]
    public async Task<IHttpActionResult> GetInventory(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var playerId = request.GetQueryParameter("player_id") ?? "";
        var items    = Database.GetCollection<MothershipInventoryItemDbObject>(true)!
            .Find(i => i.PlayerId == playerId).ToList();
        return JsonAnon(new { Results = items });
    }

    /// <summary>Adds or updates an inventory item for a player.</summary>
    [HttpPost("/v1/inventory")]
    public async Task<IHttpActionResult> AddInventoryItem(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var offers   = Database.GetCollection<MothershipOfferDbObject>(true)!.FindAll().ToList();
        var displays = Database.GetCollection<MothershipOfferDisplayDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = new { offers, featured = Array.Empty<object>(), displays } });
    }

    // ── PROGRESSION TRACKS ────────────────────────────────────────────────────

    /// <summary>Creates a new progression track.</summary>
    [HttpPost("/v1/progression")]
    public async Task<IHttpActionResult> CreateProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTrackDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a progression track by ID (automation).</summary>
    [HttpDelete("/v1/progression/automation")]
    public async Task<IHttpActionResult> AutomationDeleteProgressionTrack(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTrackBindingDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Records additional progress for a player on a track (server).</summary>
    [HttpPost("/v1/progression/server")]
    public async Task<IHttpActionResult> ServerProgression(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipProgressionTreeDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    /// <summary>Deletes a progression tree by ID (automation).</summary>
    [HttpDelete("/v1/progression-tree/automation")]
    public async Task<IHttpActionResult> AutomationDeleteProgressionTree(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { permissions = new[] { "read", "write", "execute" }, roles = new[] { "player" } });
    }

    /// <summary>Returns server/admin permission and role definitions.</summary>
    [HttpGet("/v1/permissions/server")]
    public async Task<IHttpActionResult> ServerPermissions(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        return JsonAnon(new { permissions = new[] { "admin", "read", "write", "execute", "delete" }, roles = new[] { "server", "admin" } });
    }

    // ── SERVER API KEYS ───────────────────────────────────────────────────────

    /// <summary>Creates a new server API key (automation).</summary>
    [HttpPost("/v1/server/automation/api_key")]
    public async Task<IHttpActionResult> CreateApiKey(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

        var results = Database.GetCollection<MothershipApiKeyDbObject>(true)!.FindAll().ToList();
        return JsonAnon(new { Results = results });
    }

    // ── NOTIFICATIONS ─────────────────────────────────────────────────────────

    /// <summary>Sends a notification to a player (server).</summary>
    [HttpPost("/v1/notifications/server/send")]
    public async Task<IHttpActionResult> ServerSendNotification(IHttpRequest request, IHttpResponse response)
    {
        if (!IsAdmin(request)) return JsonAnon(new { error = "Unauthorized", code = 401 }, HttpStatusCode.Unauthorized);

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
