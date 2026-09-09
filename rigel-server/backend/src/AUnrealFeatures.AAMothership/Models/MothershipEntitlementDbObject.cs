using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipEntitlementDbObject : AstraDbObject
{
    [JsonPropertyName("entitlement_id")]
    public string EntitlementId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("type")]
    public string Type { get; set; } = "DURABLE";

    [JsonPropertyName("item_class")]
    public string ItemClass { get; set; } = "";
}
