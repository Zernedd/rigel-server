using System;
using System.Runtime.Serialization;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UserRequest
{
    [JsonPropertyName("user_id"), DataMember(Name = "user_id")]
    public string UserId { get; set; }

    [JsonPropertyName("username"), DataMember(Name = "username")]
    public string Username { get; set; }

    [JsonPropertyName("discord_id"), DataMember(Name = "discord_id")]
    public string DiscordId { get; set; }

    [JsonPropertyName("platform"), DataMember(Name = "platform")]
    public string Platform { get; set; }
}
