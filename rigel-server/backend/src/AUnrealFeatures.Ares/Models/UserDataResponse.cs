using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.Serialization;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public sealed class UserDataResponse : UserData
    {
        [DataMember, JsonPropertyName("roles")]
        public List<string> Roles { get; set; }

        [DataMember, JsonPropertyName("ban")]
        public List<BanRequest> Bans { get; set; }
    }
}
