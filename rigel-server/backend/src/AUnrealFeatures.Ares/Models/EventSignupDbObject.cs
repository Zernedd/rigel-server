using AUnrealFeatures.Hosting.Database;
using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class EventSignupDbObject : AstraDbObject
{
    [JsonPropertyName("signup_id")]
    public string SignupId { get; set; }

    [JsonPropertyName("event_id")]
    public string EventId { get; set; }

    [JsonPropertyName("user_id")]
    public string UserId { get; set; }

    [JsonPropertyName("timestamp")]
    public DateTime Timestamp { get; set; }

    [JsonPropertyName("data")]
    public string? Data { get; set; }
}
