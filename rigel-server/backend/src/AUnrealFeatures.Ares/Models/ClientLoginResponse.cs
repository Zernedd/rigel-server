using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public sealed class ClientLoginResponse
    {
        [JsonPropertyName("success")]
        public bool Success { get; set; }

        [JsonPropertyName("api_key")]
        public string ApiKey { get; set; }

        [JsonPropertyName("org_scoped_id")]
        public string OrgScopedId { get; set; }

        [JsonPropertyName("server_deployments")]
        public List<ClientLoginDeployment> ServerDeployments { get; set; }
    }
}
