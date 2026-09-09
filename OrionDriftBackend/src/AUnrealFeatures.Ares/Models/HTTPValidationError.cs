using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class ValidationError
{
    [JsonPropertyName("loc")]
    public List<string> Locations { get; set; }

    [JsonPropertyName("msg")]
    public string Message { get; set; }

    [JsonPropertyName("type")]
    public string Type { get; set; }
}

public sealed class HTTPValidationError
{
    [JsonPropertyName("detail")]
    public List<ValidationError> Details { get; set; }
}
