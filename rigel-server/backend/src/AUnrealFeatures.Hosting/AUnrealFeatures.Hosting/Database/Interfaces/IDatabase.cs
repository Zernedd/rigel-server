using LiteDB;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database.Interfaces
{
    public interface IDatabase
    {
        IDatabase DropCollection<T>() where T : AstraDbObject;
        ILiteCollection<T>? GetCollection<T>(
            bool createIfNotExisting = true
        ) where T : AstraDbObject;
        Guid GetGuid();

        SimpleDatabaseDescriptor Descriptor { get; }
    }
}
