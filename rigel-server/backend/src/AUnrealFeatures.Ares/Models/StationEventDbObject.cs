using AUnrealFeatures.Hosting.Database;
using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class StationEventDbObject : AstraDbObject
{
    [JsonPropertyName("event_id")]
    public string EventId { get; set; }

    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("deployment_id")]
    public string? DeploymentId { get; set; }

    [JsonPropertyName("title")]
    public string Title { get; set; }

    [JsonPropertyName("description")]
    public string Description { get; set; }

    [JsonPropertyName("start_time")]
    public DateTime StartTime { get; set; }

    [JsonPropertyName("duration")]
    public int Duration { get; set; }

    [JsonPropertyName("public")]
    public bool Public { get; set; }

    [JsonPropertyName("signups_open")]
    public bool SignupsOpen { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, string> Config { get; set; } = new();
}
