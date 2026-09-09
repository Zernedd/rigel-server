using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class ServerEventResponse
{
    [JsonPropertyName("idx")]
    public int Index { get; set; }

    [JsonPropertyName("event_type")]
    public string EventType { get; set; }

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; }

    [JsonPropertyName("event_data")]
    public string EventData { get; set; }

    [JsonPropertyName("timestamp")]
    public DateTime Timestamp { get; set; }
}
