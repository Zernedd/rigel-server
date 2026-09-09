using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class StationDeploymentResponse
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("deployments")]
    public List<DeploymentExpanded> Deployments { get; set; }
}
