using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipTitleDbObject : AstraDbObject
{
    [JsonPropertyName("title_id")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("title_name")]
    public string TitleName { get; set; } = "";
}
