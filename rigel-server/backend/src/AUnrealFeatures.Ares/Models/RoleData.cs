using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using AUnrealFeatures.Hosting.Database;

namespace AUnrealFeatures.Ares.Models
{
    public class RoleData : AstraDbObject
    {
        [JsonPropertyName("role_id")]
        public string RoleId { get; set; }

        [JsonPropertyName("station_id")]
        public string StationId { get; set; }

        [JsonPropertyName("role_name")]
        public string RoleName { get; set; }

        [JsonPropertyName("role_description")]
        public string RoleDescription { get; set; }
    }

    public class RoleResponse : RoleData
    {
        [JsonPropertyName("permissions")]
        public List<string>? Permissions { get; set; }
    }

    public class RolesResponse
    {
        [JsonPropertyName("roles")]
        public List<RoleResponse> Roles { get; set; }
    }
}
