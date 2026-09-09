using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Models
{
    public class AssignPermsBody
    {
        [JsonPropertyName("permissions")]
        public List<string> Permissions { get; set; }
    }
}
