using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class FleetResponse
{
    [JsonPropertyName("fleet_id")]
    public string FleetId { get; set; }

    [JsonPropertyName("fleet_name")]
    public string FleetName { get; set; }

    [JsonPropertyName("created")]
    public DateTime Created { get; set; }

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("stations")]
    public List<FleetStationResponse>? Stations { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, object>? Config { get; set; }
}
