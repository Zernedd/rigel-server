using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipProgressionTrackBindingDbObject : AstraDbObject
{
    [JsonPropertyName("binding_id")]
    public string BindingId { get; set; } = "";

    [JsonPropertyName("track_id")]
    public string TrackId { get; set; } = "";

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; } = "";
}
