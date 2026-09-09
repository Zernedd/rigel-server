using AUnrealFeatures.Hosting.Database.Interfaces;
using Serilog;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Hosting.Database
{
    //
    // Contains all the needed data for reflection and collection
    // scanning, etc...
    //
    public sealed class AstraDatabaseBytedata : IDatabaseBytedata
    {
        private readonly IDatabase _parent;
        private readonly SimpleDatabaseDescriptor _descriptor;
        private static readonly ILogger _logger = ModuleInitialization.InitializeLogger("AstraDatabaseBytedata");

        public AstraDatabaseBytedata(IDatabase parent, SimpleDatabaseDescriptor descriptor)
        {
            _parent = parent;
            _descriptor = descriptor;

            this.CompletePath = _descriptor.path + ".meta";
            this.CollectionMetadata = new List<IDatabaseCollectionMetadata>();
            Initialize();
        }

        private byte[] CreateBytedata()
        {
            using MemoryStream memoryStream = new MemoryStream();
            using BinaryWriter binaryWriter = new BinaryWriter(memoryStream);

            binaryWriter.Write(CompletePath);

            binaryWriter.Write(CollectionMetadata.Count);
            for (int i = 0; i < CollectionMetadata.Count; i++)
            {
                binaryWriter.Write(CollectionMetadata[i].CollectionName);
                binaryWriter.Write(CollectionMetadata[i].CollectionType);
                binaryWriter.Write(CollectionMetadata[i].MaxItems);
            }

            binaryWriter.Write(CreatedAt.Ticks);
            binaryWriter.Write(LastModifiedAt.Ticks);

            return memoryStream.ToArray();
        }

        private void ReadFromBytedata(byte[] buffer)
        {
            using MemoryStream memoryStream = new MemoryStream(buffer);
            using BinaryReader binaryReader = new BinaryReader(memoryStream);

            CompletePath = binaryReader.ReadString();

            int count = binaryReader.ReadInt32();
            CollectionMetadata = new List<IDatabaseCollectionMetadata>(count);
            for (int i = 0; i < count; i++)
            {
                CollectionMetadata.Add(new AstraDatabaseCollectionMetadata
                {
                    CollectionName = binaryReader.ReadString(),
                    CollectionType = binaryReader.ReadString(),
                    MaxItems = binaryReader.ReadInt32()
                });
            }

            CreatedAt = new DateTime(binaryReader.ReadInt64(), DateTimeKind.Utc);
            LastModifiedAt = new DateTime(binaryReader.ReadInt64(), DateTimeKind.Utc);
        }

        private void Initialize()
        {
            if (!File.Exists(CompletePath))
                File.WriteAllBytes(CompletePath, CreateBytedata());
            else
            {
                // read from bytedata
                ReadFromBytedata(File.ReadAllBytes(CompletePath));
            }
        }

        public void Save()
        {
            if (!File.Exists(CompletePath))
            {
                _logger.Warning("Cannot save database because the metadata does not exist.");
                return;
            }
            
            File.WriteAllBytes(CompletePath, CreateBytedata());
        }

        public string CompletePath { get; private set; }
        public List<IDatabaseCollectionMetadata> CollectionMetadata { get; private set; }
        public DateTime CreatedAt { get; private set; } = DateTime.UtcNow;
        public DateTime LastModifiedAt { get; private set; } = DateTime.UtcNow;
    }
}
