using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class CreateRoleRequest
{
    [JsonPropertyName("role_name")]
    public string RoleName { get; set; }

    [JsonPropertyName("role_description")]
    public string RoleDescription { get; set; }
}
