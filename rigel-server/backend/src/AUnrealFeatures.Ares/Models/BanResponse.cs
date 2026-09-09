using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public sealed class BanResponse
    {
        [JsonPropertyName("bans")]
        public List<BanRequest> Bans { get; set; }
    }
}
