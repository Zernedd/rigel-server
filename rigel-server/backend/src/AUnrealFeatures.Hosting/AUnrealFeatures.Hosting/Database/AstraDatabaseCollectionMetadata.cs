using AUnrealFeatures.Hosting.Database.Interfaces;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database
{
    public sealed class AstraDatabaseCollectionMetadata : IDatabaseCollectionMetadata
    {
        public string CollectionName { get; internal set; } = "";
        public string CollectionType { get; internal set; } = "";
        public int MaxItems { get; internal set; }
    }
}
