using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public class UserApiKeyDbObject : AstraDbObject
{
    [JsonPropertyName("user_id")]
    public string UserId { get; set; }

    [JsonPropertyName("api_key")]
    public string ApiKey { get; set; }
}
