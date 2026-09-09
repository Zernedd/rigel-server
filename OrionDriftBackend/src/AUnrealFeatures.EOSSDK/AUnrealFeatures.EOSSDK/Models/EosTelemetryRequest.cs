using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosTelemetryRequest
{
    [JsonPropertyName("Events")]
    public List<EosTelemetryEvent> Events { get; set; } = new();
}

public sealed class EosTelemetryEvent
{
    [JsonPropertyName("EventName")]
    public string EventName { get; set; } = string.Empty;

    [JsonPropertyName("EventNamespace")]
    public string? EventNamespace { get; set; }

    [JsonPropertyName("DateOffset")]
    public string? DateOffset { get; set; }

    [JsonPropertyName("EventData")]
    public Dictionary<string, object>? EventData { get; set; }
}
