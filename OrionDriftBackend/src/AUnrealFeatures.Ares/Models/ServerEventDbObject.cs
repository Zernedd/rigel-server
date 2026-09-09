using AUnrealFeatures.Hosting.Database;
using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class ServerEventDbObject : AstraDbObject
{
    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; }

    [JsonPropertyName("event_type")]
    public string EventType { get; set; }

    [JsonPropertyName("event_data")]
    public string EventData { get; set; }

    [JsonPropertyName("timestamp")]
    public DateTime Timestamp { get; set; }
}
