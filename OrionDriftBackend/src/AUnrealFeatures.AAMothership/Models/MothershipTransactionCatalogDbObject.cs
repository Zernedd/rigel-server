using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipTransactionCatalogDbObject : AstraDbObject
{
    [JsonPropertyName("transaction_id")]
    public string TransactionId { get; set; } = "";

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("operations")]
    public string OperationsJson { get; set; } = "[]";
}
