using AUnrealFeatures.Hosting.Database;
using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class DeploymentDbObject : AstraDbObject
{
    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; }

    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("deployment_name")]
    public string? DeploymentName { get; set; }

    [JsonPropertyName("ip")]
    public string? IpAddress { get; set; }

    [JsonPropertyName("region")]
    public string? Region { get; set; }

    [JsonPropertyName("version")]
    public string? Version { get; set; }

    [JsonPropertyName("created")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("last_event")]
    public DateTime? LastEvent { get; set; }

    [JsonPropertyName("player_count")]
    public int PlayerCount { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, string> Config { get; set; } = new();

    [JsonIgnore]
    public List<string> ApiKeys { get; set; } = new();
}
