using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UpdateStationResponse
{
    [JsonPropertyName("station_name")]
    public string StationName { get; set; }
}
