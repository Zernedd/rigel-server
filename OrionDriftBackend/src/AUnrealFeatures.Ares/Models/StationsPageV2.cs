using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

// A2 build 29932 GET /v2/stations response. Matches SDK FStationsResponsePage
// { FString fleet_id; TArray<FStationResponse> Items }. Items reuse FleetStationResponse,
// which already mirrors the SDK FStationResponse layout (station_id/fleet_id/session_id/
// region/ip/version/player_count/config/district_populations/...).
public sealed class StationsPageV2
{
    [JsonPropertyName("fleet_id")]
    public string FleetId { get; set; } = "";

    [JsonPropertyName("items")]
    public List<FleetStationResponse> Items { get; set; } = new();
}
