using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipUserDataDbObject : AstraDbObject
{
    [JsonPropertyName("id")]
    public string DataId { get; set; } = "";

    [JsonPropertyName("user_id")]
    public string UserId { get; set; } = "";

    [JsonPropertyName("key_name")]
    public string KeyName { get; set; } = "";

    [JsonPropertyName("value")]
    public string Value { get; set; } = "";

    [JsonPropertyName("generation")]
    public int Generation { get; set; } = 1;
}
