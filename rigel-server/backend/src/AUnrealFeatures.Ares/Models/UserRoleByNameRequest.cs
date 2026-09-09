using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UserRoleByNameRequest
{
    [JsonPropertyName("role_id")]
    public string RoleId { get; set; }

    [JsonPropertyName("username")]
    public string Username { get; set; }

    [JsonPropertyName("expires_hours")]
    public int ExpiresInHours { get; set; }
}
