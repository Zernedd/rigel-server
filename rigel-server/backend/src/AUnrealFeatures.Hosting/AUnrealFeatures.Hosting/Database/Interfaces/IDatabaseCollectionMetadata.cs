using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database.Interfaces
{
    public interface IDatabaseCollectionMetadata
    {
        string CollectionName { get; }
        string CollectionType { get; }
        int MaxItems { get; }
    }
}
