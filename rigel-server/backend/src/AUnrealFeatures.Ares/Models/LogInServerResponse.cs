using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UserPermission
{
    [JsonPropertyName("station_id")]
    public string StationId { get; set; }

    [JsonPropertyName("permission")]
    public string Permission { get; set; }
}

public sealed class LogInServerResponse
{
    [JsonPropertyName("api_key")]
    public string ApiKey { get; set; }

    [JsonPropertyName("user")]
    public UserData User { get; set; }

    [JsonPropertyName("permissions")]
    public List<UserPermission> Permissions { get; set; }
}
