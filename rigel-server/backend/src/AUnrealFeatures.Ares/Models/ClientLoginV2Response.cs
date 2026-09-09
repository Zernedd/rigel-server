using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

// A2 build 29932 POST /v2/users/log_in response. Slim vs v1/legacy: the server list is NO
// longer embedded here — the client browses via GET /v2/stations separately.
// Matches SDK FClientLoginV2Response { bool Success; FString api_key; FString org_scoped_id }.
public sealed class ClientLoginV2Response
{
    [JsonPropertyName("success")]
    public bool Success { get; set; }

    [JsonPropertyName("api_key")]
    public string ApiKey { get; set; } = "";

    [JsonPropertyName("org_scoped_id")]
    public string OrgScopedId { get; set; } = "";
}
