using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public sealed class ClientLoginRequest
    {
        [JsonPropertyName("nonce")]
        public string Nonce { get; set; }

        [JsonPropertyName("platform_id")]
        public string PlatformId { get; set; }

        [JsonPropertyName("userName")]
        public string Username { get; set; }

        [JsonPropertyName("platform")]
        public string Platform { get; set; }

        [JsonPropertyName("device")]
        public string Device { get; set; }
    }
}
