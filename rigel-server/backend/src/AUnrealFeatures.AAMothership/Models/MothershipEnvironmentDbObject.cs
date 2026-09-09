using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipEnvironmentDbObject : AstraDbObject
{
    [JsonPropertyName("envId")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("titleId")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("envName")]
    public string EnvName { get; set; } = "";

    [JsonPropertyName("requiredTags")]
    public List<string> RequiredTags { get; set; } = new();
}
