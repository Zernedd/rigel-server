using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosOAuthTokenResponse
{
    [JsonPropertyName("access_token")]
    public string AccessToken { get; set; } = string.Empty;

    [JsonPropertyName("token_type")]
    public string TokenType { get; set; } = "bearer";

    [JsonPropertyName("expires_at")]
    public string ExpiresAt { get; set; } = string.Empty;

    [JsonPropertyName("expires_in")]
    public int ExpiresIn { get; set; }

    [JsonPropertyName("nonce")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? Nonce { get; set; }

    [JsonPropertyName("features")]
    public string[] Features { get; set; } = Array.Empty<string>();

    [JsonPropertyName("organization_id")]
    public string OrganizationId { get; set; } = string.Empty;

    [JsonPropertyName("product_id")]
    public string ProductId { get; set; } = string.Empty;

    [JsonPropertyName("sandbox_id")]
    public string SandboxId { get; set; } = string.Empty;

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; } = string.Empty;

    [JsonPropertyName("organization_user_id")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? OrganizationUserId { get; set; }

    [JsonPropertyName("product_user_id")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? ProductUserId { get; set; }

    [JsonPropertyName("product_user_id_created")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public bool? ProductUserIdCreated { get; set; }

    [JsonPropertyName("id_token")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? IdToken { get; set; }

    [JsonPropertyName("client_id")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? ClientId { get; set; }
}
