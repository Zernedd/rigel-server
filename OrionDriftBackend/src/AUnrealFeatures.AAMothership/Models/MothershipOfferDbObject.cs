using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipOfferDbObject : AstraDbObject
{
    [JsonPropertyName("offer_id")]
    public string OfferId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("titleId")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("envId")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("transaction_id")]
    public string TransactionId { get; set; } = "";

    [JsonPropertyName("bundle_pricing")]
    public string BundlePricingJson { get; set; } = "{}";

    [JsonPropertyName("discount_percent")]
    public int DiscountPercent { get; set; } = 0;
}
