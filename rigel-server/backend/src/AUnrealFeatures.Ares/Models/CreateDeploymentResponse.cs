using System;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

public sealed class CreateDeploymentResponse
{
    [JsonPropertyName("api_key")]
    public string ApiKey { get; set; }

    [JsonPropertyName("deployment_id")]
    public string DeploymentId { get; set; }
}
