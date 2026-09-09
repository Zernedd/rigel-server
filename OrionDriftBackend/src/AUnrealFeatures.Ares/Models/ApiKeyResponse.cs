using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public class ApiKeyResponse
    {
        [JsonPropertyName("api_key")]
        public string ApiKey { get; set; }
    }
}
