using AUnrealFeatures.Hosting.Database;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.AAMothership.Models;

public sealed class MothershipFriendEntryDbObject : AstraDbObject
{
    [JsonPropertyName("owner_id")]
    public string OwnerId { get; set; } = "";

    [JsonPropertyName("friend_link_id")]
    public string FriendLinkId { get; set; } = "";

    [JsonPropertyName("username")]
    public string UserName { get; set; } = "";

    [JsonPropertyName("room_id")]
    public string RoomId { get; set; } = "";

    [JsonPropertyName("zone")]
    public string Zone { get; set; } = "";

    [JsonPropertyName("region")]
    public string Region { get; set; } = "";

    [JsonPropertyName("is_public")]
    public bool IsPublic { get; set; } = true;

    [JsonPropertyName("created")]
    public string Created { get; set; } = "";
}
