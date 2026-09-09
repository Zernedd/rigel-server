using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipMuteDbObject : AstraDbObject
{
    [JsonPropertyName("mute_id")]
    public string MuteId { get; set; } = "";

    [JsonPropertyName("player_id")]
    public string PlayerId { get; set; } = "";

    [JsonPropertyName("duration_minutes")]
    public int DurationMinutes { get; set; }

    [JsonPropertyName("created_at")]
    public DateTime CreatedAt { get; set; }
}
