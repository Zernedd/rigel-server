using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipLinkDbObject : AstraDbObject
{
    [JsonPropertyName("link_id")]
    public string LinkId { get; set; } = "";

    [JsonPropertyName("platform")]
    public string Platform { get; set; } = "";

    [JsonPropertyName("target_player")]
    public string TargetPlayer { get; set; } = "";

    [JsonPropertyName("other_token")]
    public string OtherToken { get; set; } = "";

    [JsonPropertyName("is_primary")]
    public bool IsPrimary { get; set; }

    [JsonPropertyName("created_at")]
    public DateTime CreatedAt { get; set; }
}
