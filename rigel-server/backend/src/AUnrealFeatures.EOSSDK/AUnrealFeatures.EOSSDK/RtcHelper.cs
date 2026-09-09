using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.EOSSDK;

internal static class RtcHelper
{
    const string API_KEY    = "pavlov_key";
    const string API_SECRET = "pavlov_rtc_secret_12345678901234";
    internal const string SERVER_URL = "ws://94.72.120.104:7880";

    internal static string RoomName(string lobbyId) => $"lobby-{lobbyId}";

    internal static string MakeToken(string roomName, string puid)
    {
        var header  = B64Url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
        var exp     = DateTimeOffset.UtcNow.AddHours(6).ToUnixTimeSeconds();
        var nbf     = DateTimeOffset.UtcNow.AddSeconds(-1).ToUnixTimeSeconds();
        var payload = B64Url(JsonSerializer.Serialize(new
        {
            iss   = API_KEY,
            sub   = puid,
            jti   = puid,
            exp,
            nbf,
            video = new { room = roomName, roomJoin = true, canPublish = true, canSubscribe = true }
        }));
        var sig = B64Url(HmacSha256Bytes($"{header}.{payload}", API_SECRET));
        return $"{header}.{payload}.{sig}";
    }

    static string B64Url(string s) => B64Url(Encoding.UTF8.GetBytes(s));
    static string B64Url(byte[] b) => Convert.ToBase64String(b).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    static byte[] HmacSha256Bytes(string data, string key)
    {
        using var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(key));
        return hmac.ComputeHash(Encoding.UTF8.GetBytes(data));
    }
}
