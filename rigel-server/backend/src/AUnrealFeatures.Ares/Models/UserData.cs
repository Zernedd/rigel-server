using AUnrealFeatures.Hosting.Database;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.Serialization;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public class UserData : AstraDbObject
    {
        [DataMember, JsonPropertyName("user_id")]
        public string UserId { get; set; }

        [DataMember, JsonPropertyName("username")]
        public string Username { get; set; }

        [DataMember, JsonPropertyName("discord_id")]
        public string DiscordId { get; set; }

        [DataMember, JsonPropertyName("platform")]
        public string Platform { get; set; }

        [DataMember, JsonPropertyName("created")]
        public DateTime CreatedAt { get; set; }

        [DataMember, JsonPropertyName("last_login")]
        public DateTime LastLogin { get; set; }
    }
}
