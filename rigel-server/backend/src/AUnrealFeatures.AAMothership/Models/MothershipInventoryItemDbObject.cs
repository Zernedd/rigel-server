using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipInventoryItemDbObject : AstraDbObject
{
    [JsonPropertyName("player_id")]
    public string PlayerId { get; set; } = "";

    [JsonPropertyName("item_id")]
    public string ItemId { get; set; } = "";

    [JsonPropertyName("item_data")]
    public string ItemDataJson { get; set; } = "{}";
}
