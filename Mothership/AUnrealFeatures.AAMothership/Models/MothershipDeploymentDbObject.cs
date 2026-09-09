using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipDeploymentDbObject : AstraDbObject
{
    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; } = "";

    [JsonPropertyName("title_id")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("env_id")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("requiredTags")]
    public List<string> RequiredTags { get; set; } = new();
}
