using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosTurnCredentialsResponse
{
    [JsonPropertyName("username")]
    public string Username { get; set; } = string.Empty;

    [JsonPropertyName("password")]
    public string Password { get; set; } = string.Empty;

    [JsonPropertyName("ttl")]
    public int Ttl { get; set; }

    [JsonPropertyName("uris")]
    public string[] Uris { get; set; } = Array.Empty<string>();
}
