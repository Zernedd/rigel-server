using AUnrealFeatures.Hosting.Database.Interfaces;
using LiteDB;
using System;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.Linq;
using System.Runtime.Serialization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Serialization;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database
{
    public abstract class AstraDbObject : IDbObject
    {
        [JsonIgnore, DataMember, Key]
        public ObjectId Id { get; set; } = ObjectId.NewObjectId();
    }
}
