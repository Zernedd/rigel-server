using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class StationResponse
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("station_name")]
    public string StationName { get; set; }

    [JsonPropertyName("created")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("deployments")]
    public List<ServerDeploymentResponse>? Deployments { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, string>? Config { get; set; }
}
