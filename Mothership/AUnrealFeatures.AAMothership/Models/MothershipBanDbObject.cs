using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipBanDbObject : AstraDbObject
{
    [JsonPropertyName("ban_id")]
    public string BanId { get; set; } = "";

    [JsonPropertyName("title_id")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("env_id")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("player_id")]
    public string PlayerId { get; set; } = "";

    [JsonPropertyName("category")]
    public int Category { get; set; }

    [JsonPropertyName("reason")]
    public string Reason { get; set; } = "";

    [JsonPropertyName("duration_minutes")]
    public int DurationMinutes { get; set; }

    [JsonPropertyName("org_wide")]
    public bool OrgWide { get; set; }

    [JsonPropertyName("metadata")]
    public string Metadata { get; set; } = "";

    [JsonPropertyName("created_at")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("expires_at")]
    public DateTime ExpiresAt { get; set; }
}
