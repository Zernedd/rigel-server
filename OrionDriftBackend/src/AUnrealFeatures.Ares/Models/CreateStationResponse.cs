using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class CreateStationResponse
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }
}
