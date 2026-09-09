using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipProgressionTrackDbObject : AstraDbObject
{
    [JsonPropertyName("track_id")]
    public string TrackId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("levels")]
    public string LevelsJson { get; set; } = "[]";

    [JsonPropertyName("triggers")]
    public string TriggersJson { get; set; } = "[]";
}
