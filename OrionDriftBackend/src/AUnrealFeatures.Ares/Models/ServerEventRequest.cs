using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class ServerEventRequest
{
    [JsonPropertyName("event_type")]
    public string EventType { get; set; } // >= 1 characters

    [JsonPropertyName("event_data")]
    public string EventData { get; set; } // application/json
}
