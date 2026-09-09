using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UpdateStationRequest
{
    [JsonPropertyName("station_name")]
    public string StationName { get; set; }
}
