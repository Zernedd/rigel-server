using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UserRoleByNameResponse
{
    [JsonPropertyName("success")]
    public bool Success { get; set; }

    [JsonPropertyName("user_exists")]
    public bool UserExists { get; set; }
}
