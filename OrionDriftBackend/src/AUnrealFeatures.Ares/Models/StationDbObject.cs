using AUnrealFeatures.Hosting.Database;
using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class StationDbObject : AstraDbObject
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("station_name")]
    public string StationName { get; set; }

    [JsonPropertyName("created")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("online")]
    public bool Online { get; set; }

    [JsonPropertyName("last_online")]
    public DateTime? LastOnline { get; set; }

    [JsonPropertyName("config")]
    public Dictionary<string, string> Config { get; set; } = new();
}
