using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipV2PlayerDbObject : AstraDbObject
{
    [JsonPropertyName("ExternalProviderId")]
    public string ExternalProviderId { get; set; } = "";

    [JsonPropertyName("ExternalProviderUsername")]
    public string ExternalProviderUsername { get; set; } = "";

    [JsonPropertyName("IsPrimaryId")]
    public bool IsPrimaryId { get; set; } = true;

    [JsonPropertyName("PlayerId")]
    public string PlayerId { get; set; } = "";

    [JsonPropertyName("Tags")]
    public List<string>? Tags { get; set; }

    [JsonPropertyName("Token")]
    public string Token { get; set; } = "";

    [JsonPropertyName("ExpirationTime")]
    public long ExpirationTime { get; set; }

    // Stored in DB for filtering, not returned to clients
    [JsonIgnore]
    public string ExternalService { get; set; } = "";
}
