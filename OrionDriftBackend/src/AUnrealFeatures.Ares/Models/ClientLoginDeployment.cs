using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public sealed class ClientLoginDeployment
    {
        [JsonPropertyName("deployment_id")]
        public string DeploymentId { get; set; }

        [JsonPropertyName("station_id")]
        public string StationId { get; set; }

        [JsonPropertyName("station_name")]
        public string StationName { get; set; }

        [JsonPropertyName("deployment_name")]
        public string? DeploymentName { get; set; }

        [JsonPropertyName("ip")]
        public string? IpAddress {  get; set; }

        [JsonPropertyName("is_whitelist")]
        public bool IsWhitelist { get; set; }

        [JsonPropertyName("is_public")]
        public bool IsPublic { get; set; }

        [JsonPropertyName("is_joinable")]
        public bool IsJoinable { get; set; }
    }
}
