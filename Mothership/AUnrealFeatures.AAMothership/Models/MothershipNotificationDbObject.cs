using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipNotificationDbObject : AstraDbObject
{
    [JsonPropertyName("notification_id")]
    public string NotificationId { get; set; } = "";

    [JsonPropertyName("player_id")]
    public string PlayerId { get; set; } = "";

    [JsonPropertyName("message")]
    public string Message { get; set; } = "";

    [JsonPropertyName("type")]
    public string Type { get; set; } = "";

    [JsonPropertyName("sent_at")]
    public string SentAt { get; set; } = "";
}
