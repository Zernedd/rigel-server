using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipProgressionTreeBindingDbObject : AstraDbObject
{
    [JsonPropertyName("binding_id")]
    public string BindingId { get; set; } = "";

    [JsonPropertyName("tree_id")]
    public string TreeId { get; set; } = "";

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; } = "";
}
