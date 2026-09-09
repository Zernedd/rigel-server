using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class StationPerm
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("permission")]
    public string Permission { get; set; }
}
