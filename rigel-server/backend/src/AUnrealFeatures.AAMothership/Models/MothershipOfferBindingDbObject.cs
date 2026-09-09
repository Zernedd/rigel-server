using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipOfferBindingDbObject : AstraDbObject
{
    [JsonPropertyName("offer_binding_id")]
    public string OfferBindingId { get; set; } = "";

    [JsonPropertyName("title_id")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("env_id")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; } = "";

    [JsonPropertyName("offer_display_id")]
    public string OfferDisplayId { get; set; } = "";

    [JsonPropertyName("offer_id")]
    public string OfferId { get; set; } = "";

    [JsonPropertyName("committed")]
    public bool Committed { get; set; } = false;

    [JsonPropertyName("display_index")]
    public int DisplayIndex { get; set; } = 0;
}
