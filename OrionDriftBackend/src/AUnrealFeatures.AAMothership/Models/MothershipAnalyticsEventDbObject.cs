using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipAnalyticsEventDbObject : AstraDbObject
{
    [JsonPropertyName("event_id")]
    public string EventId { get; set; } = "";

    [JsonPropertyName("platform")]
    public string Platform { get; set; } = "";

    [JsonPropertyName("timestamp")]
    public string Timestamp { get; set; } = "";

    [JsonPropertyName("data")]
    public string DataJson { get; set; } = "{}";
}
