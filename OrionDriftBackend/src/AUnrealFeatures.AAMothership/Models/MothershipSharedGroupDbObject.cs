using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipSharedGroupDbObject : AstraDbObject
{
    [JsonPropertyName("SharedGroupId")]
    public string SharedGroupId { get; set; } = "";

    [JsonPropertyName("data")]
    public Dictionary<string, string> Data { get; set; } = new();

    [JsonPropertyName("members")]
    public List<string> Members { get; set; } = new();
}
