using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class CreateStationRequest
{
    [JsonPropertyName("station_name")]
    public string StationName { get; set; }

    [JsonPropertyName("station_id")]
    public string StationId { get; set; }
}
