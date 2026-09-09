using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class UsersResponse
{
    [JsonPropertyName("users")]
    public List<UserDataResponse> Users { get; set; }
}
