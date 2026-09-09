using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipPlayerDbObject : AstraDbObject
{
    [JsonPropertyName("MothershipPlayerId")]
    public string MothershipPlayerId { get; set; } = "";

    [JsonPropertyName("ExternalAccountNickname")]
    public string ExternalAccountNickname { get; set; } = "";

    [JsonPropertyName("ExternalAccountId")]
    public string ExternalAccountId { get; set; } = "";

    [JsonPropertyName("ExpirationTime")]
    public long ExpirationTime { get; set; }

    [JsonPropertyName("Tags")]
    public List<string> Tags { get; set; } = new();

    [JsonPropertyName("Token")]
    public string Token { get; set; } = "";
}
