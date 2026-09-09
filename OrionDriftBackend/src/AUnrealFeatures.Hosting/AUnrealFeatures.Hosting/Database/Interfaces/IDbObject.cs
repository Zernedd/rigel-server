using System;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using LiteDB;

namespace AUnrealFeatures.Hosting.Database.Interfaces
{
    public interface IDbObject
    {
        [Key, BsonId]
        ObjectId Id { get; }
    }
}
