using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class EventSignupResponse
{
    [JsonPropertyName("user_signed_up")]
    public bool UserSignedUp { get; set; }

    [JsonPropertyName("signup_id")]
    public string SignupId { get; set; }
}
