using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipProgressionTreeDbObject : AstraDbObject
{
    [JsonPropertyName("tree_id")]
    public string TreeId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("nodes")]
    public string NodesJson { get; set; } = "[]";
}
