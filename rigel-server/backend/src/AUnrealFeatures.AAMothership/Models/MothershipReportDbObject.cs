using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipReportDbObject : AstraDbObject
{
    [JsonPropertyName("report_id")]
    public string ReportId { get; set; } = "";

    [JsonPropertyName("reported_user_id")]
    public string ReportedUserId { get; set; } = "";

    [JsonPropertyName("reporter_user_id")]
    public string ReporterUserId { get; set; } = "";

    [JsonPropertyName("category")]
    public int Category { get; set; }

    [JsonPropertyName("reason")]
    public string Reason { get; set; } = "";

    [JsonPropertyName("platform")]
    public string Platform { get; set; } = "";

    [JsonPropertyName("modded_client")]
    public bool ModdedClient { get; set; }

    [JsonPropertyName("metadata")]
    public string Metadata { get; set; } = "";

    [JsonPropertyName("created_at")]
    public DateTime CreatedAt { get; set; }
}
