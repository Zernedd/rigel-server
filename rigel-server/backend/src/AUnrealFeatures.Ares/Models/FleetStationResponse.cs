using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class FleetStationResponse
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("fleet_id")]
    public string FleetId { get; set; }

    [JsonPropertyName("session_id")]
    public string? SessionId { get; set; }

    [JsonPropertyName("station_name")]
    public string? StationName { get; set; }

    [JsonPropertyName("region")]
    public string? Region { get; set; }

    [JsonPropertyName("ip")]
    public string? Ip { get; set; }

    [JsonPropertyName("version")]
    public string? Version { get; set; }

    [JsonPropertyName("deployment_cl")]
    public string? DeploymentCl { get; set; }

    [JsonPropertyName("created")]
    public DateTime Created { get; set; }

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("last_event")]
    public DateTime? LastEvent { get; set; }

    [JsonPropertyName("player_count")]
    public int PlayerCount { get; set; }

    [JsonPropertyName("disabled")]
    public bool Disabled { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, object>? Config { get; set; }

    [JsonPropertyName("district_populations")]
    public object? DistrictPopulations { get; set; }
}
