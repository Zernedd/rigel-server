using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UserDataV1Response
{
    [JsonPropertyName("user_id")]
    public string UserId { get; set; }

    [JsonPropertyName("username")]
    public string Username { get; set; }

    [JsonPropertyName("discord_id")]
    public string? DiscordId { get; set; }

    [JsonPropertyName("platform")]
    public string? Platform { get; set; }

    [JsonPropertyName("created")]
    public DateTime CreatedAt { get; set; }

    [JsonPropertyName("last_login")]
    public DateTime LastLogin { get; set; }

    [JsonPropertyName("roles")]
    public List<RoleResponse>? Roles { get; set; }

    [JsonPropertyName("ban")]
    public List<BanRequest>? Bans { get; set; }

    public static UserDataV1Response FromStorage(UserDataResponse stored, List<RoleResponse>? roles, bool includeBans)
    {
        return new UserDataV1Response
        {
            UserId = stored.UserId,
            Username = stored.Username,
            DiscordId = stored.DiscordId,
            Platform = stored.Platform,
            CreatedAt = stored.CreatedAt,
            LastLogin = stored.LastLogin,
            Roles = roles,
            Bans = includeBans ? stored.Bans : null
        };
    }
}
