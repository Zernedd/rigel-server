using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipPrivacyStateDbObject : AstraDbObject
{
    [JsonPropertyName("playfab_id")]
    public string PlayFabId { get; set; } = "";

    [JsonPropertyName("privacy_state")]
    public int PrivacyState { get; set; } = 0;
}
