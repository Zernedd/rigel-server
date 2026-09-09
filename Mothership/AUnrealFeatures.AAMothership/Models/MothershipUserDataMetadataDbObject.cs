using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipUserDataMetadataDbObject : AstraDbObject
{
    [JsonPropertyName("metadata_id")]
    public string MetadataId { get; set; } = "";

    [JsonPropertyName("title_id")]
    public string TitleId { get; set; } = "";

    [JsonPropertyName("env_id")]
    public string EnvId { get; set; } = "";

    [JsonPropertyName("key_name")]
    public string KeyName { get; set; } = "";

    [JsonPropertyName("key_permissions")]
    public string KeyPermissions { get; set; } = "";

    [JsonPropertyName("privacy_notes")]
    public string PrivacyNotes { get; set; } = "";
}
