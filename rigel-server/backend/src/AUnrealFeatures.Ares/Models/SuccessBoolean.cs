using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class SuccessBoolean
{
    [JsonPropertyName("success")]
    public bool Success { get; set; }
}
