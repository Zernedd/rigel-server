using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.EOSSDK;

/// <summary>
/// Generates self-signed RS256 JWTs that match the EOS token structure.
/// The EOS SDK validates format and expiry; signature verification against
/// EOS's real public key is skipped for local private-server use.
/// </summary>
internal static class EosJwtFactory
{
    const string KID = "2022-06-14T06:17:57.047928700Z";

    // One RSA key pair per process lifetime (regenerated on restart — fine for local use)
    static readonly RSA Rsa = RSA.Create(2048);

    public static readonly string[] Features =
    {
        "Achievements", "AntiCheat", "Connect", "Leaderboards", "Lobbies",
        "Matchmaking", "Metrics", "Notifications", "PlayerDataStorage",
        "PlayerReports", "ProgressionSnapshot", "Sanctions", "Stats",
        "TitleStorage", "Voice"
    };

    // ── Public factory methods ────────────────────────────────────────────────

    /// <summary>
    /// User token — returned for grant_type=external_auth.
    /// </summary>
    public static string UserToken(
        string clientId, string productId, string sandboxId, string deploymentId,
        string orgId, string productUserId, string orgUserId,
        string nonce, string displayName, string clientIp,
        string idp = "oculus_app_id", string platformId = "0", string platformType = "other")
    {
        long iat = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        long exp = iat + 3600 * 24 * 365 * 10; // 10 years

        var account = new Dictionary<string, string>
        {
            ["idp"]         = idp,
            ["displayName"] = displayName,
            ["id"]          = platformId,
            ["plf"]         = platformType,
        };
        if (idp == "oculus_app_id")
            account["oculusOrgScopedId"] = "0";

        var payload = new Dictionary<string, object>
        {
            ["clientId"]          = clientId,
            ["productId"]         = productId,
            ["iss"]               = "eos",
            ["env"]               = "prod",
            ["nonce"]             = nonce,
            ["organizationId"]    = orgId,
            ["features"]          = Features,
            ["productUserId"]     = productUserId,
            ["organizationUserId"]= orgUserId,
            ["clientIp"]          = clientIp,
            ["deploymentId"]      = deploymentId,
            ["sandboxId"]         = sandboxId,
            ["tokenType"]         = "userToken",
            ["exp"]               = exp,
            ["iat"]               = iat,
            ["account"]           = account,
            ["jti"]               = Guid.NewGuid().ToString("N"),
        };

        return Sign(payload);
    }

    /// <summary>
    /// Client token — returned for grant_type=client_credentials.
    /// </summary>
    public static string ClientToken(
        string clientId, string productId, string sandboxId, string deploymentId, string orgId)
    {
        long iat = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        long exp = iat + 3600; // 1 hour, matching real EOS

        var payload = new Dictionary<string, object>
        {
            ["clientId"]       = clientId,
            ["productId"]      = productId,
            ["iss"]            = "eos",
            ["env"]            = "prod",
            ["organizationId"] = orgId,
            ["features"]       = Features,
            ["deploymentId"]   = deploymentId,
            ["sandboxId"]      = sandboxId,
            ["tokenType"]      = "clientToken",
            ["exp"]            = exp,
            ["iat"]            = iat,
            ["jti"]            = Guid.NewGuid().ToString("N"),
        };

        return Sign(payload);
    }

    // ── ID token (Connect) ────────────────────────────────────────────────────

    /// <summary>
    /// ID token — included in the user token response alongside access_token.
    /// </summary>
    public static string IdToken(
        string clientId, string productId, string sandboxId, string deploymentId,
        string productUserId,
        string idp = "oculus_app_id", string platformId = "0", string platformType = "other")
    {
        long iat = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        long exp = iat + 3600 * 24 * 365 * 10;

        var payload = new Dictionary<string, object>
        {
            ["aud"]         = clientId,
            ["sub"]         = productUserId,
            ["pfsid"]       = sandboxId,
            ["act"]         = new Dictionary<string, string>
            {
                ["pltfm"] = platformType,
                ["eaid"]  = platformId,
                ["eat"]   = idp,
            },
            ["pfdid"]       = deploymentId,
            ["iss"]         = "https://api.epicgames.dev/auth/v1/oauth",
            ["exp"]         = exp,
            ["tokenType"]   = "idToken",
            ["iat"]         = iat,
            ["pfpid"]       = productId,
            ["jti"]         = Guid.NewGuid().ToString("N"),
        };

        return Sign(payload);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    static string Sign(Dictionary<string, object> payload)
    {
        const string headerJson = $"{{\"kid\":\"{KID}\",\"typ\":\"JWT\",\"alg\":\"RS256\"}}";
        var payloadJson = JsonSerializer.Serialize(payload);

        var header64  = B64(headerJson);
        var payload64 = B64(payloadJson);
        var message   = $"{header64}.{payload64}";

        var sig = Rsa.SignData(
            Encoding.UTF8.GetBytes(message),
            HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1);

        return $"{message}.{B64(sig)}";
    }

    static string B64(string s) => B64(Encoding.UTF8.GetBytes(s));
    static string B64(byte[] b) =>
        Convert.ToBase64String(b).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    // ── JWKS ─────────────────────────────────────────────────────────────────

    public static object GetJwks()
    {
        var p = Rsa.ExportParameters(false);
        return new
        {
            keys = new[]
            {
                new
                {
                    kty = "RSA",
                    use = "sig",
                    alg = "RS256",
                    kid = KID,
                    n   = B64(p.Modulus!),
                    e   = B64(p.Exponent!),
                }
            }
        };
    }

    // ── Deterministic product user ID from display name ───────────────────────

    /// <summary>
    /// Generates a stable product_user_id for a given display name.
    /// Uses SHA-1 of the name, formatted like real EOS IDs (0002... prefix).
    /// </summary>
    public static string ProductUserIdFor(string displayName)
    {
        var hash = SHA1.HashData(Encoding.UTF8.GetBytes("puid:" + displayName));
        return "0002" + Convert.ToHexString(hash)[..28].ToLowerInvariant();
    }

    public static string OrgUserIdFor(string displayName)
    {
        var hash = SHA1.HashData(Encoding.UTF8.GetBytes("ouid:" + displayName));
        return "0001" + Convert.ToHexString(hash)[..28].ToLowerInvariant();
    }

    /// <summary>
    /// Extracts the client_id from the Authorization: Basic ... header.
    /// Returns the fallback value if the header is missing or malformed.
    /// </summary>
    public static string ExtractClientId(string? authHeader, string fallback)
    {
        if (string.IsNullOrEmpty(authHeader) || !authHeader.StartsWith("Basic ", StringComparison.OrdinalIgnoreCase))
            return fallback;
        try
        {
            var decoded = Encoding.UTF8.GetString(Convert.FromBase64String(authHeader[6..]));
            return decoded.Split(':')[0];
        }
        catch { return fallback; }
    }
}
