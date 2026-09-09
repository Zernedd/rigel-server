using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class DeploymentRequest
{
    [JsonPropertyName("ip")]
    public string IpAddress { get; set; }

    [JsonPropertyName("deployment_name")]
    public string DeploymentName { get; set; }

    [JsonPropertyName("region")]
    public string Region { get; set; }

    [JsonPropertyName("version")]
    public string Version { get; set; }

    [JsonPropertyName("station_id")]
    public string StationId { get; set; }
}

public sealed class DeploymentExpanded : DeploymentRequest
{
    [JsonPropertyName("created")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("last_event")]
    public DateTime LastEvent { get; set; }

    [JsonPropertyName("player_count")]
    public int PlayerCount { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, string>? Config { get; set; }
}
