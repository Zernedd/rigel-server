using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database.Interfaces
{
    public interface IDatabaseBytedata
    {
        void Save();

        string CompletePath { get; }
        List<IDatabaseCollectionMetadata> CollectionMetadata { get; }
        DateTime CreatedAt { get; }
        DateTime LastModifiedAt { get; }
    }
}
