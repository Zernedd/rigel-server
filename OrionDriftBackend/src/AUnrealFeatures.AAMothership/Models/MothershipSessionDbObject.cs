using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipSessionDbObject : AstraDbObject
{
    [JsonPropertyName("Token")]
    public string Token { get; set; } = "";

    [JsonPropertyName("PlayerId")]
    public string PlayerId { get; set; } = "";
}
